#include "../CombBLAS.h"
#include <mpi.h>
#include <sys/time.h> 
#include <iostream>
#include <functional>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>
#include "BPMaximalMatching.h"
#include "Utility.h"

#ifdef THREADED
	#ifndef _OPENMP
	#define _OPENMP
	#endif

	#include <omp.h>
    int cblas_splits = 1;
#endif

using namespace std;




template <typename PARMAT>
void Symmetricize(PARMAT & A)
{
	// boolean addition is practically a "logical or"
	// therefore this doesn't destruct any links
	PARMAT AT = A;
	AT.Transpose();
	A += AT;
}


struct VertexType
{
public:
    VertexType(int64_t p=-1, int64_t r=-1, int16_t pr=0){parent=p; root = r; prob = pr;};
    
    friend bool operator<(const VertexType & vtx1, const VertexType & vtx2 )
    {
        if(vtx1.prob==vtx2.prob) return vtx1.parent<vtx2.parent;
        else return vtx1.prob<vtx2.prob;
    };
    friend bool operator==(const VertexType & vtx1, const VertexType & vtx2 ){return vtx1.parent==vtx2.parent;};
    friend ostream& operator<<(ostream& os, const VertexType & vertex ){os << "(" << vertex.parent << "," << vertex.root << ")"; return os;};
    //private:
    int64_t parent;
    int64_t root;
    int16_t prob; // probability of selecting an edge
    
};




typedef SpParMat < int64_t, bool, SpDCCols<int64_t,bool> > PSpMat_Bool;
typedef SpParMat < int64_t, bool, SpDCCols<int32_t,bool> > PSpMat_s32p64;
typedef SpParMat < int64_t, int64_t, SpDCCols<int64_t,int64_t> > PSpMat_Int64;
typedef SpParMat < int64_t, float, SpDCCols<int64_t,float> > PSpMat_float;
void maximumMatching(PSpMat_s32p64 & Aeff, FullyDistVec<int64_t, int64_t>& mateRow2Col,
                     FullyDistVec<int64_t, int64_t>& mateCol2Row);
template <class IT, class NT>
bool isMaximalmatching(PSpMat_Int64 & A, FullyDistVec<IT,NT> & mateRow2Col, FullyDistVec<IT,NT> & mateCol2Row,
                       FullyDistSpVec<int64_t, int64_t> unmatchedRow, FullyDistSpVec<int64_t, int64_t> unmatchedCol);




/*
 Remove isolated vertices and purmute
 */
void removeIsolated(PSpMat_Int64 & A)
{
    
    int nprocs, myrank;
    MPI_Comm_size(MPI_COMM_WORLD,&nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
    
    
    FullyDistVec<int64_t, int64_t> * ColSums = new FullyDistVec<int64_t, int64_t>(A.getcommgrid());
    FullyDistVec<int64_t, int64_t> * RowSums = new FullyDistVec<int64_t, int64_t>(A.getcommgrid());
    FullyDistVec<int64_t, int64_t> nonisoRowV;	// id's of non-isolated (connected) Row vertices
    FullyDistVec<int64_t, int64_t> nonisoColV;	// id's of non-isolated (connected) Col vertices
    FullyDistVec<int64_t, int64_t> nonisov;	// id's of non-isolated (connected) vertices
    
    A.Reduce(*ColSums, Column, plus<int64_t>(), static_cast<int64_t>(0));
    A.Reduce(*RowSums, Row, plus<int64_t>(), static_cast<int64_t>(0));
    
    // this steps for general graph
    /*
     ColSums->EWiseApply(*RowSums, plus<int64_t>()); not needed for bipartite graph
     nonisov = ColSums->FindInds(bind2nd(greater<int64_t>(), 0));
     nonisov.RandPerm();	// so that A(v,v) is load-balanced (both memory and time wise)
     A.operator()(nonisov, nonisov, true);	// in-place permute to save memory
     */
    
    // this steps for bipartite graph
    nonisoColV = ColSums->FindInds(bind2nd(greater<int64_t>(), 0));
    nonisoRowV = RowSums->FindInds(bind2nd(greater<int64_t>(), 0));
    delete ColSums;
    delete RowSums;
    

    {
        nonisoColV.RandPerm();
        nonisoRowV.RandPerm();
    }
    
    
    int64_t nrows1=A.getnrow(), ncols1=A.getncol(), nnz1 = A.getnnz();
    double avgDeg1 = (double) nnz1/(nrows1+ncols1);
    
    
    A.operator()(nonisoRowV, nonisoColV, true);
    
    int64_t nrows2=A.getnrow(), ncols2=A.getncol(), nnz2 = A.getnnz();
    double avgDeg2 = (double) nnz2/(nrows2+ncols2);
    
    
    if(myrank == 0)
    {
        cout << "ncol nrows  nedges deg \n";
        cout << nrows1 << " " << ncols1 << " " << nnz1 << " " << avgDeg1 << " \n";
        cout << nrows2 << " " << ncols2 << " " << nnz2 << " " << avgDeg2 << " \n";
    }
    
    MPI_Barrier(MPI_COMM_WORLD);
    
    
}




/**
 * Create a boolean matrix A (not necessarily a permutation matrix)
 * Input: ri: a dense vector (actual values in FullyDistVec should be IT)
 *        ncol: number of columns in the output matrix A
 * Output: a boolean matrix A with m=size(ri) and n=ncol (input)
           and  A[k,ri[k]]=1
 */
template <class IT, class DER>
SpParMat<IT, bool, DER> PermMat (const FullyDistVec<IT,IT> & ri, const IT ncol)
{
    
    IT procsPerRow = ri.commGrid->GetGridCols();	// the number of processor in a row of processor grid
    IT procsPerCol = ri.commGrid->GetGridRows();	// the number of processor in a column of processor grid
    
    
    IT global_nrow = ri.TotalLength();
    IT global_ncol = ncol;
    IT m_perprocrow = global_nrow / procsPerRow;
    IT n_perproccol = global_ncol / procsPerCol;
    

    // The indices for FullyDistVec are offset'd to 1/p pieces
    // The matrix indices are offset'd to 1/sqrt(p) pieces
    // Add the corresponding offset before sending the data

    vector< vector<IT> > rowid(procsPerRow); // rowid in the local matrix of each vector entry
    vector< vector<IT> > colid(procsPerRow); // colid in the local matrix of each vector entry
    
    IT locvec = ri.arr.size();	// nnz in local vector
    IT roffset = ri.RowLenUntil(); // the number of vector elements in this processor row before the current processor
    for(typename vector<IT>::size_type i=0; i< (unsigned)locvec; ++i)
    {
        if(ri.arr[i]>=0 && ri.arr[i]<ncol) // this specialized for matching. TODO: make it general purpose by passing a function
        {
            IT rowrec = (n_perproccol!=0) ? std::min(ri.arr[i] / n_perproccol, procsPerRow-1) : (procsPerRow-1);
            // ri's numerical values give the colids and its local indices give rowids
            rowid[rowrec].push_back( i + roffset);
            colid[rowrec].push_back(ri.arr[i] - (rowrec * n_perproccol));
        }
       
    }
    
    

    int * sendcnt = new int[procsPerRow];
    int * recvcnt = new int[procsPerRow];
    for(IT i=0; i<procsPerRow; ++i)
    {
        sendcnt[i] = rowid[i].size();
    }
    
    MPI_Alltoall(sendcnt, 1, MPI_INT, recvcnt, 1, MPI_INT, ri.commGrid->GetRowWorld()); // share the counts
    
    int * sdispls = new int[procsPerRow]();
    int * rdispls = new int[procsPerRow]();
    partial_sum(sendcnt, sendcnt+procsPerRow-1, sdispls+1);
    partial_sum(recvcnt, recvcnt+procsPerRow-1, rdispls+1);
    IT p_nnz = accumulate(recvcnt,recvcnt+procsPerRow, static_cast<IT>(0));
    

    IT * p_rows = new IT[p_nnz];
    IT * p_cols = new IT[p_nnz];
    IT * senddata = new IT[locvec];
    for(int i=0; i<procsPerRow; ++i)
    {
        copy(rowid[i].begin(), rowid[i].end(), senddata+sdispls[i]);
        vector<IT>().swap(rowid[i]);	// clear memory of rowid
    }
    MPI_Alltoallv(senddata, sendcnt, sdispls, MPIType<IT>(), p_rows, recvcnt, rdispls, MPIType<IT>(), ri.commGrid->GetRowWorld());
    
    for(int i=0; i<procsPerRow; ++i)
    {
        copy(colid[i].begin(), colid[i].end(), senddata+sdispls[i]);
        vector<IT>().swap(colid[i]);	// clear memory of colid
    }
    MPI_Alltoallv(senddata, sendcnt, sdispls, MPIType<IT>(), p_cols, recvcnt, rdispls, MPIType<IT>(), ri.commGrid->GetRowWorld());
    delete [] senddata;
    
    tuple<IT,IT,bool> * p_tuples = new tuple<IT,IT,bool>[p_nnz];
    for(IT i=0; i< p_nnz; ++i)
    {
        p_tuples[i] = make_tuple(p_rows[i], p_cols[i], 1);
    }
    DeleteAll(p_rows, p_cols);
    
    
    // Now create the local matrix
    IT local_nrow = ri.MyRowLength();
    int my_proccol = ri.commGrid->GetRankInProcRow();
    IT local_ncol = (my_proccol<(procsPerCol-1))? (n_perproccol) : (global_ncol - (n_perproccol*(procsPerCol-1)));
    
    // infer the concrete type SpMat<IT,IT>
    typedef typename create_trait<DER, IT, bool>::T_inferred DER_IT;
    DER_IT * PSeq = new DER_IT();
    PSeq->Create( p_nnz, local_nrow, local_ncol, p_tuples);		// deletion of tuples[] is handled by SpMat::Create
    
    SpParMat<IT,bool,DER_IT> P (PSeq, ri.commGrid);
    //PSpMat_Bool P (PSeq, ri.commGrid);
    return P;
}




/**
 * Create a boolean matrix A (not necessarily a permutation matrix)
 * Input: ri: a dense vector (actual values in FullyDistVec should be IT)
 *        ncol: number of columns in the output matrix A
 * Output: a boolean matrix A with m=size(ri) and n=ncol (input)
 and  A[k,ri[k]]=1
 */

template <class IT, class NT, class DER, typename _UnaryOperation>
SpParMat<IT, bool, DER> PermMat1 (const FullyDistSpVec<IT,NT> & ri, const IT ncol, _UnaryOperation __unop)
{
    
    IT procsPerRow = ri.commGrid->GetGridCols();	// the number of processor in a row of processor grid
    IT procsPerCol = ri.commGrid->GetGridRows();	// the number of processor in a column of processor grid
    
    
    IT global_nrow = ri.TotalLength();
    IT global_ncol = ncol;
    IT m_perprocrow = global_nrow / procsPerRow;
    IT n_perproccol = global_ncol / procsPerCol;
    
    
    // The indices for FullyDistVec are offset'd to 1/p pieces
    // The matrix indices are offset'd to 1/sqrt(p) pieces
    // Add the corresponding offset before sending the data
    
    vector< vector<IT> > rowid(procsPerRow); // rowid in the local matrix of each vector entry
    vector< vector<IT> > colid(procsPerRow); // colid in the local matrix of each vector entry
    
    IT locvec = ri.num.size();	// nnz in local vector
    IT roffset = ri.RowLenUntil(); // the number of vector elements in this processor row before the current processor
    for(typename vector<IT>::size_type i=0; i< (unsigned)locvec; ++i)
    {
        IT val = __unop(ri.num[i]);
        if(val>=0 && val<ncol)
        {
            IT rowrec = (n_perproccol!=0) ? std::min(val / n_perproccol, procsPerRow-1) : (procsPerRow-1);
            // ri's numerical values give the colids and its local indices give rowids
            rowid[rowrec].push_back( i + roffset); // ************************************ this is not right, it should be (ri.ind[i]+offset)
            colid[rowrec].push_back(val - (rowrec * n_perproccol));
        }
    }
    
    
    
    int * sendcnt = new int[procsPerRow];
    int * recvcnt = new int[procsPerRow];
    for(IT i=0; i<procsPerRow; ++i)
    {
        sendcnt[i] = rowid[i].size();
    }
    
    MPI_Alltoall(sendcnt, 1, MPI_INT, recvcnt, 1, MPI_INT, ri.commGrid->GetRowWorld()); // share the counts
    
    int * sdispls = new int[procsPerRow]();
    int * rdispls = new int[procsPerRow]();
    partial_sum(sendcnt, sendcnt+procsPerRow-1, sdispls+1);
    partial_sum(recvcnt, recvcnt+procsPerRow-1, rdispls+1);
    IT p_nnz = accumulate(recvcnt,recvcnt+procsPerRow, static_cast<IT>(0));
    
    
    IT * p_rows = new IT[p_nnz];
    IT * p_cols = new IT[p_nnz];
    IT * senddata = new IT[locvec];
    for(int i=0; i<procsPerRow; ++i)
    {
        copy(rowid[i].begin(), rowid[i].end(), senddata+sdispls[i]);
        vector<IT>().swap(rowid[i]);	// clear memory of rowid
    }
    MPI_Alltoallv(senddata, sendcnt, sdispls, MPIType<IT>(), p_rows, recvcnt, rdispls, MPIType<IT>(), ri.commGrid->GetRowWorld());
    
    for(int i=0; i<procsPerRow; ++i)
    {
        copy(colid[i].begin(), colid[i].end(), senddata+sdispls[i]);
        vector<IT>().swap(colid[i]);	// clear memory of colid
    }
    MPI_Alltoallv(senddata, sendcnt, sdispls, MPIType<IT>(), p_cols, recvcnt, rdispls, MPIType<IT>(), ri.commGrid->GetRowWorld());
    delete [] senddata;
    
    tuple<IT,IT,bool> * p_tuples = new tuple<IT,IT,bool>[p_nnz];
    for(IT i=0; i< p_nnz; ++i)
    {
        p_tuples[i] = make_tuple(p_rows[i], p_cols[i], 1);
    }
    DeleteAll(p_rows, p_cols);
    
    
    // Now create the local matrix
    IT local_nrow = ri.MyRowLength();
    int my_proccol = ri.commGrid->GetRankInProcRow();
    IT local_ncol = (my_proccol<(procsPerCol-1))? (n_perproccol) : (global_ncol - (n_perproccol*(procsPerCol-1)));
    
    // infer the concrete type SpMat<IT,IT>
    typedef typename create_trait<DER, IT, bool>::T_inferred DER_IT;
    DER_IT * PSeq = new DER_IT();
    PSeq->Create( p_nnz, local_nrow, local_ncol, p_tuples);		// deletion of tuples[] is handled by SpMat::Create
    
    SpParMat<IT,bool,DER_IT> P (PSeq, ri.commGrid);
    //PSpMat_Bool P (PSeq, ri.commGrid);
    return P;
}

void ShowUsage()
{
    int myrank;
    MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
    if(myrank == 0)
    {
        cout << "\n-------------- usage --------------\n";
        cout << "Usage (random matrix): ./bpmm <er|g500|ssca> <Scale> <EDGEFACTOR> <init><diropt><prune><graft>\n";
        cout << "Usage (input matrix): ./bpmm <input> <matrix> <init><diropt><prune><graft>\n\n";
        
        cout << " \n-------------- meaning of arguments ----------\n";
        cout << "** er: Erdos-Renyi, g500: Graph500 benchmark, ssca: SSCA benchmark\n";
        cout << "** scale: matrix dimention is 2^scale\n";
        cout << "** edgefactor: average degree of vertices\n";
        cout << "** (optional) init : maximal matching algorithm used to initialize\n ";
        cout << "      none: noinit, greedy: greedy init , ks: Karp-Sipser, dmd: dynamic mindegree\n";
        cout << "       default: none\n";
        cout << "** (optional) diropt: employ direction-optimized BFS\n" ;
        cout << "** (optional) prune: discard trees as soon as an augmenting path is found\n" ;
        cout << "** (optional) graft: employ tree grafting\n" ;
        cout << "(order of optional arguments does not matter)\n";
        
        cout << " \n-------------- examples ----------\n";
        cout << "Example: mpirun -np 4 ./bpmm g500 18 16" << endl;
        cout << "Example: mpirun -np 4 ./bpmm g500 18 16 ks diropt graft" << endl;
        cout << "Example: mpirun -np 4 ./bpmm input cage12.mtx ks diropt graft\n" << endl;
    }
}

void GetOptions(char* argv[], int argc, int & init, bool & diropt, bool & prune, bool & graft)
{
    string allArg="";
    for(int i=0; i<argc; i++)
    {
        allArg += string(argv[i]);
    }
    if(allArg.find("diropt")!=string::npos)
        diropt = true;
    if(allArg.find("prune")!=string::npos)
        prune = true;
    if(allArg.find("graft")!=string::npos)
        graft = true;
    if(allArg.find("greedy")!=string::npos)
        init = GREEDY;
    else if(allArg.find("ks")!=string::npos)
        init = KARP_SIPSER;
    else if(allArg.find("dmd")!=string::npos)
        init = DMD;
    else
        init = NO_INIT;
    
}


int main(int argc, char* argv[])
{
	
    // ------------ initialize MPI ---------------
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE)
    {
        printf("ERROR: The MPI library does not have MPI_THREAD_SERIALIZED support\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    int nprocs, myrank;
	MPI_Comm_size(MPI_COMM_WORLD,&nprocs);
	MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
    if(argc < 3)
    {
        ShowUsage();
        MPI_Finalize();
        return -1;
    }

    int init = NO_INIT;
    bool diropt=false, prune=false, graft=false;
    
    // ------------ Process input arguments and build matrix ---------------
	{
        
        PSpMat_Bool * ABool;
        PSpMat_s32p64 ALocalT;
        ostringstream tinfo;
        double t01, t02;
        if(string(argv[1]) == string("input")) // input option
        {
            ABool = new PSpMat_Bool();
            string filename(argv[2]);
            SpParHelper::Print("Reading input matrix....\n");
            t01 = MPI_Wtime();
            ABool->ParallelReadMM(filename);
            t02 = MPI_Wtime();
            ABool->PrintInfo();
            tinfo.str("");
            tinfo << "Reader took " << t02-t01 << " seconds" << endl;
            SpParHelper::Print(tinfo.str());
            
            /*
            // random permutations for load balance
            if(permute)
            {
                if(A->getnrow() == A->getncol())
                {
                    if(p.TotalLength()!=A->getnrow())
                    {
                        SpParHelper::Print("Generating random permutation vector.\n");
                        p.iota(A->getnrow(), 0);
                        p.RandPerm();
                    }
                    SpParHelper::Print("Perfoming random permuation of matrix.\n");
                    (*A)(p,p,true);// in-place permute to save memory
                    ostringstream tinfo1;
                    tinfo1 << "Permutation took " << MPI_Wtime()-t02 << " seconds" << endl;
                    SpParHelper::Print(tinfo1.str());
                }
                else
                {
                    SpParHelper::Print("nrow != ncol. Can not apply symmetric permutation.\n");
                }
            }
             */
            
            GetOptions(argv+3, argc-3, init, diropt, prune, graft);

        }
        else if(argc < 4)
        {
            ShowUsage();
            MPI_Finalize();
            return -1;
        }
        else
        {
            unsigned scale = (unsigned) atoi(argv[2]);
            unsigned EDGEFACTOR = (unsigned) atoi(argv[3]);
            double initiator[4];
            if(string(argv[1]) == string("er"))
            {
                initiator[0] = .25;
                initiator[1] = .25;
                initiator[2] = .25;
                initiator[3] = .25;
            }
            else if(string(argv[1]) == string("g500"))
            {
                initiator[0] = .57;
                initiator[1] = .19;
                initiator[2] = .19;
                initiator[3] = .05;
            }
            else if(string(argv[1]) == string("ssca"))
            {
                initiator[0] = .6;
                initiator[1] = .4/3;
                initiator[2] = .4/3;
                initiator[3] = .4/3;
            }
            else
            {
                if(myrank == 0)
                    printf("The input type - %s - is not recognized.\n", argv[2]);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
            
            SpParHelper::Print("Generating input matrix....\n");
            t01 = MPI_Wtime();
            DistEdgeList<int64_t> * DEL = new DistEdgeList<int64_t>();
            DEL->GenGraph500Data(initiator, scale, EDGEFACTOR, true, true);
            ABool = new PSpMat_Bool(*DEL, false);
            delete DEL;
            t02 = MPI_Wtime();
            ABool->PrintInfo();
            tinfo.str("");
            tinfo << "Generator took " << t02-t01 << " seconds" << endl;
            SpParHelper::Print(tinfo.str());
            
            Symmetricize(*ABool);
            SpParHelper::Print("Generated matrix symmetricized....\n");
            ABool->PrintInfo();
            
            GetOptions(argv+4, argc-4, init, diropt, prune, graft);

        }

        
		
		

        
        
        //int64_t removed  = ABool->RemoveLoops(); // loop means an edges (i,i+NU) in a bipartite graph
        
       
        // remove isolated vertice if necessary
        
        //greedyMatching(*ABool);
        // maximumMatching(*ABool);
        //maximumMatchingSimple(*ABool);
        
        
        
        
        //PSpMat_Bool A1;
        //A1.ReadDistribute("amazon0312.mtx", 0);	// read it from file
        //A1.ReadDistribute("coPapersDBLP.mtx", 0);	// read it from file
        //A1.PrintInfo();
        
        
        
        //////
        /*
        // Remove Isolated vertice
        FullyDistVec<int64_t, int64_t> * ColSums = new FullyDistVec<int64_t, int64_t>(A.getcommgrid());
        FullyDistVec<int64_t, int64_t> * RowSums = new FullyDistVec<int64_t, int64_t>(A.getcommgrid());
        FullyDistVec<int64_t, int64_t> nonisoRowV;	// id's of non-isolated (connected) Row vertices
        FullyDistVec<int64_t, int64_t> nonisoColV;	// id's of non-isolated (connected) Col vertices
        FullyDistVec<int64_t, int64_t> nonisov;	// id's of non-isolated (connected) vertices
        
        A1.Reduce(*ColSums, Column, plus<int64_t>(), static_cast<int64_t>(0));
        A1.Reduce(*RowSums, Row, plus<int64_t>(), static_cast<int64_t>(0));
        //ColSums->EWiseApply(*RowSums, plus<int64_t>());
        nonisov = ColSums->FindInds(bind2nd(greater<int64_t>(), 0));
        nonisoColV = ColSums->FindInds(bind2nd(greater<int64_t>(), 0));
        nonisoRowV = RowSums->FindInds(bind2nd(greater<int64_t>(), 0));
        //nonisoColV.iota(A.getncol(), 0);
        nonisov.RandPerm();	// so that A(v,v) is load-balanced (both memory and time wise)
        nonisoColV.RandPerm();
        nonisoRowV.RandPerm();
        
        delete ColSums;
        delete RowSums;
        
        //A(nonisoColV, nonisoColV, true);	// in-place permute to save memory
        A.operator()(nonisoRowV, nonisoColV, true);
        /////
        */
        
        
     
        PSpMat_s32p64 A = *ABool;
        PSpMat_s32p64 AT = A;
        AT.Transpose();
        
#ifdef _OPENMP
#pragma omp parallel
        {
            cblas_splits = omp_get_num_threads()*1;
        }
        tinfo.str("");
        tinfo << "Threading activated with " << cblas_splits << " threads" << endl;
        SpParHelper::Print(tinfo.str());
        A.ActivateThreading(cblas_splits); // note: crash on empty matrix
        AT.ActivateThreading(cblas_splits);
#endif


        
        
        //if(argc>=4 && static_cast<unsigned>(atoi(argv[3]))==1)
        //    removeIsolated(A);
        
        tinfo.str("");
        tinfo << "\n---------------------------------\n";
        tinfo << "Calling maximum-cardinality matching with options: " << endl;
        tinfo << " init: ";
        if(init == NO_INIT) tinfo << " no-init ";
        if(init == KARP_SIPSER) tinfo << " Karp-Sipser, ";
        if(init == DMD) tinfo << " dynamic mindegree, ";
        if(init == GREEDY) tinfo << " greedy, ";
        if(diropt) tinfo << " direction-optimized BFS, ";
        if(prune) tinfo << " tree pruning, ";
        if(graft) tinfo << " tree grafting ";
        tinfo << "\n---------------------------------\n\n";
        SpParHelper::Print(tinfo.str());
        
        
        FullyDistVec<int64_t, int64_t> mateRow2Col ( A.getcommgrid(), A.getnrow(), (int64_t) -1);
        FullyDistVec<int64_t, int64_t> mateCol2Row ( A.getcommgrid(), A.getncol(), (int64_t) -1);
        //if(argc>=3 && static_cast<unsigned>(atoi(argv[2]))==1)
        //greedyMatching(A, mateRow2Col, mateCol2Row);
        
        
        //hybrid(A, AT, mateRow2Col, mateCol2Row, init, true);
        //KS(A, AT, mateRow2Col, mateCol2Row);
        
        
        //A1.Transpose();
        //varify_matching(*ABool);
        
        
        maximumMatching(A, mateRow2Col, mateCol2Row);
        
        int64_t ncols=A.getncol();
        int64_t matched = mateCol2Row.Count([](int64_t mate){return mate!=-1;});
        if(myrank==0)
        {
            cout << "matched %cols\n";
            printf("%lld %lf \n",matched, 100*(double)matched/(ncols));
        }
        //mateRow2Col.DebugPrint();
	}
	MPI_Finalize();
	return 0;
}



// It is not easy to write these two inverts using matvec.
// The first invert is accessing mate, hence can be implemented by multipying with matching matrix
// The second invert access parents, which can not be implemented with simple matvec.


void Augment1(FullyDistVec<int64_t, int64_t>& mateRow2Col, FullyDistVec<int64_t, int64_t>& mateCol2Row,
             FullyDistVec<int64_t, int64_t>& parentsRow, FullyDistVec<int64_t, int64_t>& leaves)
{
 
    int64_t nrow = mateRow2Col.TotalLength();
    int64_t ncol = mateCol2Row.TotalLength();
    FullyDistSpVec<int64_t, int64_t> col(leaves, [](int64_t leaf){return leaf!=-1;});
    FullyDistSpVec<int64_t, int64_t> row(mateRow2Col.getcommgrid(), nrow);
    FullyDistSpVec<int64_t, int64_t> nextcol(col.getcommgrid(), ncol);
 
    while(col.getnnz()!=0)
    {
     
        row = col.Invert(nrow);
        
        row.SelectApply(parentsRow, [](int64_t parent){return true;},
                        [](int64_t root, int64_t parent){return parent;});
        

        col = row.Invert(ncol); // children array

        nextcol = col.SelectApplyNew(mateCol2Row, [](int64_t mate){return mate!=-1;}, [](int64_t child, int64_t mate){return mate;});
        
        mateRow2Col.Set(row);
        mateCol2Row.Set(col);
        col = nextcol;
        
    }
}



template <typename IT, typename NT>
void Augment(FullyDistVec<int64_t, int64_t>& mateRow2Col, FullyDistVec<int64_t, int64_t>& mateCol2Row,
             FullyDistVec<int64_t, int64_t>& parentsRow, FullyDistVec<int64_t, int64_t>& leaves)
{

    MPI_Win win_mateRow2Col, win_mateCol2Row, win_parentsRow;
    MPI_Win_create(&mateRow2Col.arr[0], mateRow2Col.LocArrSize() * sizeof(int64_t), sizeof(int64_t), MPI_INFO_NULL, mateRow2Col.commGrid->GetWorld(), &win_mateRow2Col);
    MPI_Win_create(&mateCol2Row.arr[0], mateCol2Row.LocArrSize() * sizeof(int64_t), sizeof(int64_t), MPI_INFO_NULL, mateCol2Row.commGrid->GetWorld(), &win_mateCol2Row);
    MPI_Win_create(&parentsRow.arr[0], parentsRow.LocArrSize() * sizeof(int64_t), sizeof(int64_t), MPI_INFO_NULL, parentsRow.commGrid->GetWorld(), &win_parentsRow);

    //cout<< "Leaves: " ;
    //leaves.DebugPrint();
    //parentsRow.DebugPrint();
    
    MPI_Win_fence(0, win_mateRow2Col);
    MPI_Win_fence(0, win_mateCol2Row);
    MPI_Win_fence(0, win_parentsRow);

    int64_t row, col=100, nextrow;
    int owner_row, owner_col;
    IT locind_row, locind_col;
    int myrank;
    
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
    
#ifdef _OPENMP
//#pragma omp for // multiple thread can do their own one sided communication, what happens in receiver??
#endif
    for(IT i=0; i<leaves.LocArrSize(); i++)
    {
        int depth=0;
        row = leaves.arr[i];
        while(row != - 1)
        {
            
            owner_row = mateRow2Col.Owner(row, locind_row);
            MPI_Win_lock(MPI_LOCK_SHARED, owner_row, 0, win_parentsRow);
            MPI_Get(&col, 1, MPIType<NT>(), owner_row, locind_row, 1, MPIType<NT>(), win_parentsRow);
            MPI_Win_unlock(owner_row, win_parentsRow);
            
            owner_col = mateCol2Row.Owner(col, locind_col);
            MPI_Win_lock(MPI_LOCK_SHARED, owner_col, 0, win_mateCol2Row);
            MPI_Fetch_and_op(&row, &nextrow, MPIType<NT>(), owner_col, locind_col, MPI_REPLACE, win_mateCol2Row);
            MPI_Win_unlock(owner_col, win_mateCol2Row);
            
            MPI_Win_lock(MPI_LOCK_SHARED, owner_row, 0, win_mateRow2Col);
            MPI_Put(&col, 1, MPIType<NT>(), owner_row, locind_row, 1, MPIType<NT>(), win_mateRow2Col);
            MPI_Win_unlock(owner_row, win_mateRow2Col); // we need this otherwise col might get overwritten before communication!
            row = nextrow;
            
        }
    }

    MPI_Win_fence(0, win_mateRow2Col);
    MPI_Win_fence(0, win_mateCol2Row);
    MPI_Win_fence(0, win_parentsRow);
    
    MPI_Win_free(&win_mateRow2Col);
    MPI_Win_free(&win_mateCol2Row);
    MPI_Win_free(&win_parentsRow);
}




void prune(FullyDistSpVec<int64_t, VertexType>& fringeRow, FullyDistSpVec<int64_t, int64_t>& temp1)
{
    int64_t ncol = temp1.TotalLength();
    PSpMat_Bool MB;
    MB = PermMat1<int64_t, VertexType, SpDCCols<int64_t,bool>>(fringeRow, ncol, [](VertexType vtx){return vtx.root;});
    //PSpMat_Int64  M = Mbool; // matching matrix
    FullyDistSpVec<int64_t, int64_t> remove(fringeRow.getcommgrid(), ncol);
    //SpMV<SelectMinSRing1>(MB, temp1, remove, false);
    SpMV<Select2ndMinSR<bool, int64_t>>(MB, temp1, remove, false);
    fringeRow.Setminus(remove);
}



void maximumMatching(PSpMat_s32p64 & A, FullyDistVec<int64_t, int64_t>& mateRow2Col,
                     FullyDistVec<int64_t, int64_t>& mateCol2Row)
{
    
    int nprocs, myrank;
    MPI_Comm_size(MPI_COMM_WORLD,&nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
    
    int64_t nrow = A.getnrow();
    int64_t ncol = A.getncol();
    //FullyDistSpVec<int64_t, VertexType> unmatchedCol(A.getcommgrid(), ncol);
    
    FullyDistSpVec<int64_t, VertexType> temp(A.getcommgrid(), ncol);
    FullyDistSpVec<int64_t, int64_t> temp1(A.getcommgrid(), ncol);
    FullyDistSpVec<int64_t, VertexType> fringeRow(A.getcommgrid(), nrow);
    FullyDistSpVec<int64_t, VertexType> umFringeRow(A.getcommgrid(), nrow);
    FullyDistSpVec<int64_t, int64_t> umFringeRow1(A.getcommgrid(), nrow);
    
    
    vector<vector<double> > timing;
    double t1, time_search, time_augment, time_phase;
    
    bool matched = true;
    int phase = 0;
    int totalLayer = 0;
    int64_t numUnmatchedCol;
    
    while(matched)
    {
        time_phase = MPI_Wtime();
        
        PSpMat_Bool Mbool = PermMat<int64_t, SpDCCols<int64_t,bool>>(mateCol2Row, nrow);

#ifdef _OPENMP
        if(Mbool.getnnz()>cblas_splits)
            Mbool.ActivateThreading(cblas_splits);
#endif

        
        vector<double> phase_timing(8,0);
        FullyDistVec<int64_t, int64_t> leaves ( A.getcommgrid(), nrow, (int64_t) -1);
        FullyDistVec<int64_t, int64_t> parentsRow ( A.getcommgrid(), nrow, (int64_t) -1); // it needs to be cleared after each phase
        
        FullyDistVec<int64_t, int64_t> rootsRow ( A.getcommgrid(), nrow, (int64_t) -1); // just for test
        FullyDistSpVec<int64_t, VertexType> fringeCol(A.getcommgrid(), ncol);
        
        
        fringeCol  = EWiseApply<VertexType>(fringeCol, mateCol2Row,
                                            [](VertexType vtx, int64_t mate){return vtx;},
                                            [](VertexType vtx, int64_t mate){return mate==-1;},
                                            true, VertexType()); // root & parent both =-1
        
        fringeCol.ApplyInd([](VertexType vtx, int64_t idx){return VertexType(idx,idx);}); //  root & parent both equal to index
        //fringeCol.DebugPrint();
        
        ++phase;
        numUnmatchedCol = fringeCol.getnnz();
        int64_t tt;
        int layer = 0;
        
        double test1=0, test2=0, test3=0;
        
        time_search = MPI_Wtime();
        while(fringeCol.getnnz() > 0)
        {
            layer++;
            t1 = MPI_Wtime();
            

            SpMV<Select2ndMinSR<bool, VertexType>>(A, fringeCol, fringeRow, false);
            phase_timing[0] += MPI_Wtime()-t1;
            
            
            
            // remove vertices already having parents
            t1 = MPI_Wtime();
            //fringeRow.Select(parentsRow, [](int64_t parent){return parent==-1;}); //much faster
            fringeRow = EWiseApply_threaded<VertexType>(fringeRow, parentsRow,
                                                        [](VertexType vtx, int64_t parent, bool a, bool b){return vtx;},
                                                        [](VertexType vtx, int64_t parent, bool a, bool b){return parent==-1;},
                                                        false, VertexType(), true);
            
            // Set parent pointer
            parentsRow.EWiseApply(fringeRow,
                                  [](int64_t dval, VertexType svtx, bool a, bool b){return svtx.parent;},
                                  [](int64_t dval, VertexType svtx, bool a, bool b){return true;},
                                  false, VertexType(), false);
            
            
            umFringeRow1 = EWiseApply_threaded<int64_t>(fringeRow, mateRow2Col,
                                      [](VertexType vtx, int64_t mate, bool a, bool b){return vtx.root;},
                                      [](VertexType vtx, int64_t mate, bool a, bool b){return mate==-1;},
                                      false, VertexType(), true);
            
            
            
            phase_timing[1] += MPI_Wtime()-t1;
            t1 = MPI_Wtime();
            
            
            // get the unique leaves
            if(umFringeRow1.getnnz()>0)
            {
                //TODO: merge into one with RMA??? RMA useful because leaves is dense. So need to send count/displacement
                temp1 = umFringeRow1.Invert(ncol);
                leaves.Set(temp1);
                //leaves.GSet(umFringeRow1,
                 //           [](int64_t valRoot, int64_t idxLeaf){return valRoot;},
                   //         [](int64_t valRoot, int64_t idxLeaf){return idxLeaf;});
            }
    
            
            phase_timing[2] += MPI_Wtime()-t1;
            
            
            //fringeRow.SelectApply(mateRow2Col, [](int64_t mate){return mate!=-1;},
            //                      [](VertexType vtx, int64_t mate){return VertexType(mate, vtx.root);});
            fringeRow = EWiseApply_threaded<VertexType>(fringeRow, mateRow2Col,
                                                        [](VertexType vtx, int64_t mate, bool a, bool b){return VertexType(mate, vtx.root);},
                                                        [](VertexType vtx, int64_t mate, bool a, bool b){return mate!=-1;},
                                                        false, VertexType(), true);
            
            
            //TODO: experiment prunning
            t1 = MPI_Wtime();
            if(temp1.getnnz()>0 )
            {
                //prune(fringeRow, temp1); // option1
                fringeRow.FilterByVal (temp1,[](VertexType vtx){return vtx.root;}); //option2
            }
            double tprune = MPI_Wtime()-t1;
            phase_timing[3] += tprune;
            
            
            t1 = MPI_Wtime();
            
            if(fringeRow.getnnz() > 0)
            {
                SpMV<Select2ndMinSR<bool, VertexType>>(Mbool, fringeRow, fringeCol, false);
            }
            else break;
            phase_timing[4] += MPI_Wtime()-t1;
            
            
        }
        time_search = MPI_Wtime() - time_search;
        phase_timing[5] += time_search;
        
        
        int64_t numMatchedCol = leaves.Count([](int64_t leaf){return leaf!=-1;});
        time_augment = MPI_Wtime();
        if (numMatchedCol== 0) matched = false;
        else
        {
            Augment<int64_t,int64_t>(mateRow2Col, mateCol2Row,parentsRow, leaves);
            //Augment1(mateRow2Col, mateCol2Row,parentsRow, leaves);
        }
        time_augment = MPI_Wtime() - time_augment;
        phase_timing[6] += time_augment;
        
        
        time_phase = MPI_Wtime() - time_phase;
        phase_timing[7] += time_phase;
        timing.push_back(phase_timing);
        
        //ostringstream tinfo;
        //tinfo << "Phase: " << phase << " layers:" << layer << " Unmatched Columns: " << numUnmatchedCol << " Matched: " << numMatchedCol << " Time: "<< time_phase << " ::: "  << test1 << " , " << test2 << "\n";
        //tinfo << test1 << "  " << test2 << "\n";
        //SpParHelper::Print(tinfo.str());
        totalLayer += layer;
        
    }
    
      
    //isMaximalmatching(A, mateRow2Col, mateCol2Row, unmatchedRow, unmatchedCol);
    //isMatching(mateCol2Row, mateRow2Col); //todo there is a better way to check this
    
    
    // print statistics
    if(myrank == 0)
    {
        cout << endl;
        cout << "========================================================================\n";
        cout << "                                     BFS Search                       \n";
        cout << "===================== ==================================================\n";
        cout  << "Phase Layer    UMCol   SpMV EWOpp CmUqL  Prun CmMC   BFS   Aug   Total\n";
        cout << "===================== ===================================================\n";
        
        vector<double> totalTimes(timing[0].size(),0);
        int nphases = timing.size();
        for(int i=0; i<timing.size(); i++)
        {
            //printf(" %3d   ", i+1);
            for(int j=0; j<timing[i].size(); j++)
            {
                totalTimes[j] += timing[i][j];
                //timing[i][j] /= timing[i].back();
                //printf("%.2lf  ", timing[i][j]);
            }
            
            //printf("\n");
        }
        
        double combTime = totalTimes.back();
        printf(" %3d  %3d  %8lld   ", nphases, totalLayer/nphases, numUnmatchedCol);
        for(int j=0; j<totalTimes.size()-1; j++)
        {
            printf("%.2lf  ", totalTimes[j]);
        }
        printf("%.2lf\n", combTime);
        
        //cout << "=================== total timing ===========================\n";
        //for(int i=0; i<totalTimes.size(); i++)
        //    cout<<totalTimes[i] << " ";
        //cout << endl;
    }
    
    
    
}




