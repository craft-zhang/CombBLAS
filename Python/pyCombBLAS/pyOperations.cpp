#include "pyOperations.h"
#include <iostream>

namespace op{

/**************************\
| UNARY OPERATIONS
\**************************/


#define DECL_UNARY_STRUCT(name, operation) 							\
	template<typename T>											\
	struct name : public ConcreteUnaryFunction<T>					\
	{																\
		const T operator()(const T& x) const						\
		{															\
			operation;												\
		}															\
	};
	
#define DECL_UNARY_FUNC(structname, name, operation)				\
	DECL_UNARY_STRUCT(structname, operation)						\
	UnaryFunction* name()											\
	{																\
		return new UnaryFunction(new structname<int64_t>());			\
	}																

DECL_UNARY_FUNC(identity_s, identity, return x;)
DECL_UNARY_FUNC(negate_s, negate, return -x;)
DECL_UNARY_FUNC(bitwise_not_s, bitwise_not, return ~x;)
DECL_UNARY_FUNC(logical_not_s, logical_not, return !x;)
DECL_UNARY_FUNC(abs_s, abs, return (x < 0) ? -x : x;)


// Slightly un-standard ops:

template<typename T>
struct set_s: public ConcreteUnaryFunction<T>
{
	set_s(T myvalue): value(myvalue) {};
	/** @returns value regardless of x */
	const T operator()(const T& x) const
	{
		return value;
	} 
	T value;
};

UnaryFunction* set(int64_t val)
{
	return new UnaryFunction(new set_s<int64_t>(val));
}

template<typename T>
struct safemultinv_s : public ConcreteUnaryFunction<T>
{
	const T operator()(const T& x) const
	{
		T inf = std::numeric_limits<T>::max();
		return (x == 0) ? inf:(1/x);
	}
};

UnaryFunction* safemultinv() {
	return new UnaryFunction(new safemultinv_s<int64_t>());
}


/**************************\
| BINARY OPERATIONS
\**************************/

#define DECL_BINARY_STRUCT(name, operation) 						\
	template<typename T>											\
	struct name : public ConcreteBinaryFunction<T>				\
	{																\
		const T operator()(const T& x, const T& y) const					\
		{															\
			operation;												\
		}															\
	};
	
#define DECL_BINARY_FUNC(structname, name, operation)				\
	DECL_BINARY_STRUCT(structname, operation)						\
	BinaryFunction* name()											\
	{																\
		return new BinaryFunction(new structname<int64_t>());		\
	}																



DECL_BINARY_FUNC(plus_s, plus, return x+y;)
DECL_BINARY_FUNC(minus_s, minus, return x-y;)
DECL_BINARY_FUNC(multiplies_s, multiplies, return x*y;)
DECL_BINARY_FUNC(divides_s, divides, return x/y;)
DECL_BINARY_FUNC(modulus_s, modulus, return x % y;)

DECL_BINARY_FUNC(max_s, max, return std::max<int64_t>(x, y);)
DECL_BINARY_FUNC(min_s, min, return std::min<int64_t>(x, y);)

DECL_BINARY_FUNC(bitwise_and_s, bitwise_and, return x & y;)
DECL_BINARY_FUNC(bitwise_or_s, bitwise_or, return x | y;)
DECL_BINARY_FUNC(bitwise_xor_s, bitwise_xor, return x ^ y;)
DECL_BINARY_FUNC(logical_and_s, logical_and, return x && y;)
DECL_BINARY_FUNC(logical_or_s, logical_or, return x || y;)
DECL_BINARY_FUNC(logical_xor_s, logical_xor, return (x || y) && !(x && y);)

DECL_BINARY_FUNC(equal_to_s, equal_to, return x == y;)
DECL_BINARY_FUNC(not_equal_to_s, not_equal_to, return x != y;)
DECL_BINARY_FUNC(greater_s, greater, return x > y;)
DECL_BINARY_FUNC(less_s, less, return x < y;)
DECL_BINARY_FUNC(greater_equal_s, greater_equal, return x >= y;)
DECL_BINARY_FUNC(less_equal_s, less_equal, return x <= y;)

/**************************\
| GLUE OPERATIONS
\**************************/

// BIND
//////////////////////////////////

template<typename T>
struct bind_s : public ConcreteUnaryFunction<T>
{
	int which;
	T bindval;
	ConcreteBinaryFunction<T>* op;
	
	bind_s(ConcreteBinaryFunction<T>* opin, int w, T val): op(opin), which(w), bindval(val) {}
	
	const T operator()(const T& x) const
	{
		if (which == 1)
			return (*op)(bindval, x);
		else
			return (*op)(x, bindval);
	}
};

UnaryFunction* bind1st(BinaryFunction* op, int64_t val)
{
	return new UnaryFunction(new bind_s<int64_t>(op->op, 1, val));
	//return new UnaryFunction(bind1st(op->op, val));
}

UnaryFunction* bind2nd(BinaryFunction* op, int64_t val)
{
	return new UnaryFunction(new bind_s<int64_t>(op->op, 2, val));
	//return new UnaryFunction(bind2nd(op->op, val));
}

// COMPOSE
//////////////////////////////////

// for some reason the regular STL compose1() cannot be found, so doing this manually
template<typename T>
struct compose1_s : public ConcreteUnaryFunction<T>
{
	ConcreteUnaryFunction<T> *f, *g;
	
	compose1_s(ConcreteUnaryFunction<T>* fin, ConcreteUnaryFunction<T>* gin): f(fin), g(gin) {}
	
	const T operator()(const T& x) const
	{
		return (*f)((*g)(x));
	}
};

UnaryFunction* compose1(UnaryFunction* f, UnaryFunction* g) // h(x) is the same as f(g(x))
{
	//return new UnaryFunction(compose1(f->op, g->op));
	return new UnaryFunction(new compose1_s<int64_t>(f->op, g->op));
}


template<typename T>
struct compose2_s : public ConcreteUnaryFunction<T>
{
	ConcreteBinaryFunction<T> *f;
	ConcreteUnaryFunction<T> *g1, *g2;
	
	compose2_s(ConcreteBinaryFunction<T>* fin, ConcreteUnaryFunction<T>* g1in, ConcreteUnaryFunction<T>* g2in): f(fin), g1(g1in), g2(g2in) {}
	
	const T operator()(const T& x) const
	{
		return (*f)( (*g1)(x), (*g2)(x) );
	}
};

UnaryFunction* compose2(BinaryFunction* f, UnaryFunction* g1, UnaryFunction* g2) // h(x) is the same as f(g1(x), g2(x))
{
	//return new BinaryFunction(compose2(f->op, g1->op, g2->op));
	return new UnaryFunction(new compose2_s<int64_t>(f->op, g1->op, g2->op));
}

// NOT
//////////////////////////////////

// Standard STL not1() returns a predicate object, not a unary_function.
// We may want to do it that way as well, but to avoid type problems we keep 'not' as a plain unary function for now.
template<typename T>
struct unary_not_s : public ConcreteUnaryFunction<T>
{
	ConcreteUnaryFunction<T>* op;

	unary_not_s(ConcreteUnaryFunction<T>* operation): op(operation) {}

	const T operator()(const T& x) const
	{
		return !(*op)(x);
	}
};

UnaryFunction* not1(UnaryFunction* f)
{
	return new UnaryFunction(new unary_not_s<int64_t>(f->op));
}


template<typename T>
struct binary_not_s : public ConcreteBinaryFunction<T>
{
	ConcreteBinaryFunction<T>* op;

	binary_not_s(ConcreteBinaryFunction<T>* operation): op(operation) {}

	const T operator()(const T& x, const T& y) const
	{
		return !(*op)(x, y);
	}
};

BinaryFunction* not2(BinaryFunction* f)
{
	return new BinaryFunction(new binary_not_s<int64_t>(f->op));
}

} // namespace op
