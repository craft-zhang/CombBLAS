#include "pyOperationsObj.h"
#include <iostream>
#include <math.h>
#include <cstdlib>
#include <Python.h>

namespace op{

/**************************\
| UNARY OPERATIONS
\**************************/


UnaryFunctionObj unaryObj(PyObject *pyfunc)
{
	return UnaryFunctionObj(pyfunc);
}


UnaryPredicateObj unaryObjPred(PyObject *pyfunc)
{
	return UnaryPredicateObj(pyfunc);
}


// Slightly un-standard ops:
#if 0
template<typename T>
struct set_s: public ConcreteUnaryFunction<T>
{
	set_s(T myvalue): value(myvalue) {};
	/** @returns value regardless of x */
	T operator()(const T& x) const
	{
		return value;
	} 
	T value;
};

UnaryFunction set(Obj2* val)
{
	return UnaryFunction(new set_s<Obj2>(Obj2(*val)));
}

UnaryFunction set(Obj1* val)
{
	return UnaryFunction(new set_s<Obj1>(Obj1(*val)));
}
#endif



/**************************\
| BINARY OPERATIONS
\**************************/


BinaryFunctionObj binaryObj(PyObject *pyfunc, bool comm)
{
	// assumed to be associative but not commutative
	return BinaryFunctionObj(pyfunc, true, comm);
}

BinaryPredicateObj binaryObjPred(PyObject *pyfunc)
{
	// assumed to be associative but not commutative
	return BinaryPredicateObj(pyfunc);
}

/**************************\
| METHODS
\**************************/
BinaryFunctionObj* BinaryFunctionObj_MPI_Interface::currentlyApplied = NULL;
MPI_Op BinaryFunctionObj_MPI_Interface::staticMPIop;
	
void BinaryFunctionObj_MPI_Interface::apply(void * invec, void * inoutvec, int * len, MPI_Datatype *datatype)
{
	if (*datatype == MPIType< doubleint >())
		applyWorker(static_cast<doubleint*>(invec), static_cast<doubleint*>(inoutvec), len);
	else if (*datatype == MPIType< Obj1 >())
		applyWorker(static_cast<Obj1*>(invec), static_cast<Obj1*>(inoutvec), len);
	else if (*datatype == MPIType< Obj2 >())
		applyWorker(static_cast<Obj2*>(invec), static_cast<Obj2*>(inoutvec), len);
	else
	{
		throw string("There is an internal error in applying a BinaryFunctionObj: Unknown datatype.");
	}
}

extern "C" void BinaryFunctionObjApplyWrapper(void * invec, void * inoutvec, int * len, MPI_Datatype *datatype)
{
        BinaryFunctionObj_MPI_Interface::apply(invec, inoutvec, len, datatype);
}

MPI_Op* BinaryFunctionObj_MPI_Interface::mpi_op()
{
	return &staticMPIop;
}

void BinaryFunctionObj::getMPIOp()
{
	if (BinaryFunctionObj_MPI_Interface::currentlyApplied != NULL)
	{
		throw string("There is an internal error in creating an MPI version of a BinaryFunctionObj: Conflict between two BFOs.");
	}
	else if (BinaryFunctionObj_MPI_Interface::currentlyApplied == this)
	{
		return;
	}

	BinaryFunctionObj_MPI_Interface::currentlyApplied = this;
	int commutable_flag = int(commutable);
	MPI_Op_create((MPI_User_function *)BinaryFunctionObjApplyWrapper, commutable_flag, &BinaryFunctionObj_MPI_Interface::staticMPIop);
}

void BinaryFunctionObj::releaseMPIOp()
{
	if (BinaryFunctionObj_MPI_Interface::currentlyApplied == this)
		BinaryFunctionObj_MPI_Interface::currentlyApplied = NULL;
}

void clear_BinaryFunctionObj_currentlyApplied()
{
	BinaryFunctionObj_MPI_Interface::currentlyApplied = NULL;
}

/**************************\
| SEMIRING
\**************************/

//template <>
SemiringObj* SemiringObj::currentlyApplied = NULL;

void clear_SemiringObj_currentlyApplied()
{
	SemiringObj::currentlyApplied = NULL;
}
		
SemiringObj::SemiringObj(PyObject *add, PyObject *multiply, PyObject* left_filter_py, PyObject* right_filter_py)
	: type(CUSTOM)
{
	//Py_INCREF(pyfunc_add);
	//Py_INCREF(pyfunc_multiply);
	
	binfunc_add = new BinaryFunctionObj(add, true, true);
	binfunc_mul = new BinaryFunctionObj(multiply, true, true);
	own_add_mul = true;
	
	if (left_filter_py != NULL && Py_None != left_filter_py)
		left_filter = new UnaryPredicateObj(left_filter_py);
	else
		left_filter = NULL;

	if (right_filter_py != NULL && Py_None != right_filter_py)
		right_filter = new UnaryPredicateObj(right_filter_py);
	else
		right_filter = NULL;
	
	own_left_filter = true;
	own_right_filter = true;
}

SemiringObj::SemiringObj(BinaryFunctionObj *add, BinaryFunctionObj *multiply)
	: type(CUSTOM)//, pyfunc_add(add), pyfunc_multiply(multiply), binfunc_add(&binary(add))
{
	//Py_INCREF(pyfunc_add);
	//Py_INCREF(pyfunc_multiply);
	
	binfunc_add = add;
	binfunc_mul = multiply;
	own_add_mul = false;
	
	left_filter = NULL;
	right_filter = NULL;
}

SemiringObj::~SemiringObj()
{
	//Py_XDECREF(pyfunc_add);
	//Py_XDECREF(pyfunc_multiply);
	if (binfunc_add != NULL && own_add_mul)
		delete binfunc_add;
	if (binfunc_mul != NULL && own_add_mul)
		delete binfunc_mul;
	if (left_filter != NULL && own_left_filter)
		delete left_filter;
	if (right_filter != NULL && own_right_filter)
		delete right_filter;
	//assert(currentlyApplied != this);
}

void SemiringObj::setLeftFilter(PyObject* left_filter_py)
{
	if (left_filter_py != NULL && Py_None != left_filter_py)
	{
		left_filter = new UnaryPredicateObj(left_filter_py);
		own_left_filter = true;
	}
	else
	{
		if (left_filter != NULL && own_left_filter)
			delete left_filter;
		left_filter = NULL;
	}
}

void SemiringObj::setLeftFilter(UnaryPredicateObj *left_filter_in)
{
	if (left_filter != NULL && own_left_filter)
		delete left_filter;
	left_filter = left_filter_in;
	own_left_filter = false;
}

void SemiringObj::setRightFilter(PyObject* right_filter_py)
{
	if (right_filter_py != NULL && Py_None != right_filter_py)
	{
		right_filter = new UnaryPredicateObj(right_filter_py);
		own_right_filter = true;
	}
	else
	{
		if (right_filter != NULL && own_right_filter)
			delete right_filter;
		right_filter = NULL;
	}
}

void SemiringObj::setRightFilter(UnaryPredicateObj *right_filter_in)
{
	if (right_filter_in != NULL && own_right_filter)
		delete right_filter_in;
	right_filter = right_filter_in;
	own_right_filter = false;
}

void SemiringObj::enableSemiring()
{
	if (currentlyApplied != NULL)
	{
		throw string("There is an internal error in selecting a SemiringObj: Conflict between two Semirings.");
	}
	currentlyApplied = this;
	binfunc_add->getMPIOp();
}

void SemiringObj::disableSemiring()
{
	binfunc_add->releaseMPIOp();
	currentlyApplied = NULL;
}
/*
doubleint SemiringObj::add(const doubleint & arg1, const doubleint & arg2)
{
	PyObject *arglist;
	PyObject *result;
	double dres = 0;
	
	arglist = Py_BuildValue("(d d)", arg1.d, arg2.d);    // Build argument list
	result = PyEval_CallObject(pyfunc_add, arglist);     // Call Python
	Py_DECREF(arglist);                                  // Trash arglist
	if (result) {                                        // If no errors, return double
		dres = PyFloat_AsDouble(result);
	}
	Py_XDECREF(result);
	return doubleint(dres);
}

doubleint SemiringObj::multiply(const doubleint & arg1, const doubleint & arg2)
{
	PyObject *arglist;
	PyObject *result;
	double dres = 0;
	
	arglist = Py_BuildValue("(d d)", arg1.d, arg2.d);         // Build argument list
	result = PyEval_CallObject(pyfunc_multiply, arglist);     // Call Python
	Py_DECREF(arglist);                                       // Trash arglist
	if (result) {                                             // If no errors, return double
		dres = PyFloat_AsDouble(result);
	}
	Py_XDECREF(result);
	return doubleint(dres);
}

void SemiringObj::axpy(doubleint a, const doubleint & x, doubleint & y)
{
	y = add(y, multiply(a, x));
}

*/

SemiringObj TimesPlusSemiringObj()
{
	return SemiringObj(SemiringObj::TIMESPLUS);
}
/*
SemiringObj MinPlusSemiringObj()
{
	return SemiringObj(SemiringObj::PLUSMIN);
}*/

SemiringObj SecondMaxSemiringObj()
{
	return SemiringObj(SemiringObj::SECONDMAX);
}

} // namespace op
