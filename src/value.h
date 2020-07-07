#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <iostream>

// Workaround for https://bugreports.qt-project.org/browse/QTBUG-22829
#ifndef Q_MOC_RUN
#include <boost/variant.hpp>
#include <boost/lexical_cast.hpp>
#include <glib.h>
#endif

#include "Assignment.h"
#include "memory.h"

class tostring_visitor;
class tostream_visitor;
class Context;
class Expression;

class QuotedString : public std::string
{
public:
	QuotedString() : std::string() {}
	QuotedString(const std::string &s) : std::string(s) {}
};
std::ostream &operator<<(std::ostream &stream, const QuotedString &s);

class Filename : public QuotedString
{
public:
	Filename() : QuotedString() {}
	Filename(const std::string &f) : QuotedString(f) {}
};
std::ostream &operator<<(std::ostream &stream, const Filename &filename);

class RangeType {
private:
	double begin_val;
	double step_val;
	double end_val;
	
public:
	static constexpr uint32_t MAX_RANGE_STEPS = 10000;

	enum class type_t { RANGE_TYPE_BEGIN, RANGE_TYPE_RUNNING, RANGE_TYPE_END };

	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag ;
		using value_type        = double;
		using difference_type   = void;
		using reference         = const value_type&;
		using pointer           = const value_type*;
		iterator(const RangeType &range, type_t type);
		iterator& operator++();
		reference operator*();
		pointer operator->();
		bool operator==(const iterator& other) const;
		bool operator!=(const iterator& other) const;
	private:
		const RangeType &range;
		double val;
		type_t type;
		const uint32_t num_values;
		uint32_t i_step;
		void update_type();
	};
	
	RangeType(const RangeType &) = delete; // never copy, move instead
	RangeType& operator=(const RangeType &) = delete; // never copy, move instead
	RangeType(RangeType&&) = default; // default move constructor
	RangeType& operator=(RangeType&&) = default; // default move assignment

	explicit RangeType(double begin, double end)
		: begin_val(begin), step_val(1.0), end_val(end) {}
	
	explicit RangeType(double begin, double step, double end)
		: begin_val(begin), step_val(step), end_val(end) {}
	
	bool operator==(const RangeType &other) const {
		auto n1 = this->numValues();
		auto n2 = other.numValues();
		if (n1 == 0) return n2 == 0;
		if (n2 == 0) return false;
		return this == &other ||
			(this->begin_val == other.begin_val &&
			 this->step_val == other.step_val &&
			 n1 == n2);
	}

	bool operator<(const RangeType &other) const {
		auto n1 = this->numValues();
		auto n2 = other.numValues();
		if (n1 == 0) return 0 < n2;
		if (n2 == 0) return false;
		return this->begin_val < other.begin_val ||
			(this->begin_val == other.begin_val &&
				(this->step_val < other.step_val || (this->step_val == other.step_val && n1 < n2))
			);
	}
	
	bool operator<=(const RangeType &other) const {
		auto n1 = this->numValues();
		auto n2 = other.numValues();
		if (n1 == 0) return true; // (0 <= n2) is always true 
		if (n2 == 0) return false;
		return this->begin_val < other.begin_val ||
			(this->begin_val == other.begin_val &&
				(this->step_val < other.step_val || (this->step_val == other.step_val && n1 <= n2))
			);
	}

	bool operator>(const RangeType &other) const {
		auto n1 = this->numValues();
		auto n2 = other.numValues();
		if (n2 == 0) return n1 > 0;
		if (n1 == 0) return false;
		return this->begin_val > other.begin_val ||
			(this->begin_val == other.begin_val &&
				(this->step_val > other.step_val || (this->step_val == other.step_val && n1 > n2))
			);
	}

	bool operator>=(const RangeType &other) const {
		auto n1 = this->numValues();
		auto n2 = other.numValues();
		if (n2 == 0) return true; // (n1 >= 0) is always true
		if (n1 == 0) return false;
		return this->begin_val > other.begin_val ||
			(this->begin_val == other.begin_val &&
				(this->step_val > other.step_val || (this->step_val == other.step_val && n1 >= n2))
			);
	}

	double begin_value() const { return begin_val; }
	double step_value() const { return step_val; }
	double end_value() const { return end_val; }
	
	iterator begin() const { return iterator(*this, type_t::RANGE_TYPE_BEGIN); }
	iterator end() const{ return iterator(*this, type_t::RANGE_TYPE_END); }

	/// return number of values, max uint32_t value if step is 0 or range is infinite
	uint32_t numValues() const;

	friend class chr_visitor;
	friend class tostring_visitor;
	friend class tostream_visitor;
	friend class bracket_visitor;
	friend std::ostream& operator<<(std::ostream& stream, const RangeType& f);
};

template <typename T>
class ValuePtr {
private:
	explicit ValuePtr(const std::shared_ptr<T> &val_in) : value(val_in) { }
public:
	ValuePtr(T&& value) : value(std::make_shared<T>(std::move(value))) { }
	ValuePtr clone() const { return ValuePtr(value); }

	const T& operator*() const { return *value; }
	const T* operator->() const { return value.get(); }
	bool operator==(const ValuePtr& other) const { return *value == *other; }
	bool operator!=(const ValuePtr& other) const { return !(*this == other); }
	bool operator< (const ValuePtr& other) const { return *value <  *other; }
	bool operator> (const ValuePtr& other) const { return *value >  *other; }
	bool operator<=(const ValuePtr& other) const { return *value <= *other; }
	bool operator>=(const ValuePtr& other) const { return *value >= *other; }

private:
	std::shared_ptr<T> value;
};

using RangePtr = ValuePtr<RangeType>;

class str_utf8_wrapper
{
private:
	// store the cached length in glong, paired with its string
	struct str_utf8_t {
		static constexpr glong LENGTH_UNKNOWN = -1;
		str_utf8_t() { }
		str_utf8_t(const std::string& s) : u8str(s) { }
		str_utf8_t(const char* cstr) : u8str(cstr) { }
		str_utf8_t(const char* cstr, size_t size, glong u8len) : u8str(cstr, size), u8len(u8len) { }
		const std::string u8str;
		glong u8len = LENGTH_UNKNOWN;
	};
	// private constructor for copying members
	explicit str_utf8_wrapper(const shared_ptr<str_utf8_t> &str_in) : str_ptr(str_in) { }

public:
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag ;
		using value_type        = str_utf8_wrapper;
		using difference_type   = void;
		using reference         = const str_utf8_wrapper&;
		using pointer           = const str_utf8_wrapper*;
		iterator() noexcept : ptr(&nullterm) {} // DefaultConstructible
		iterator(const str_utf8_wrapper& str, bool get_end=false) noexcept;
		iterator& operator++();
		value_type operator*();
		bool operator==(const iterator& other) const;
		bool operator!=(const iterator& other) const;
	private:
		size_t char_len();
		static const char nullterm = '\0';
		const char *ptr;
		size_t len;
	};

	iterator begin() const { return iterator(*this); }
	iterator end() const{ return iterator(*this, true); }
	str_utf8_wrapper() : str_ptr(make_shared<str_utf8_t>()) { }
	str_utf8_wrapper( const std::string& s ) : str_ptr(make_shared<str_utf8_t>(s)) { }
	str_utf8_wrapper(const char* cstr) : str_ptr(make_shared<str_utf8_t>(cstr)) { }
	// for enumerating single utf8 chars from iterator
	str_utf8_wrapper(const char* cstr, size_t clen) : str_ptr(make_shared<str_utf8_t>(cstr,clen,1)) { }
	str_utf8_wrapper(const str_utf8_wrapper &) = delete; // never copy, move instead
	str_utf8_wrapper& operator=(const str_utf8_wrapper &) = delete; // never copy, move instead
	str_utf8_wrapper(str_utf8_wrapper&&) noexcept = default; // default move constructor
	str_utf8_wrapper& operator=(str_utf8_wrapper&&) noexcept = default; // default move assignment

	// makes a copy of shared_ptr
	str_utf8_wrapper clone() const noexcept { return str_utf8_wrapper(this->str_ptr); }

	bool operator==(const str_utf8_wrapper &rhs) const noexcept { return this->str_ptr->u8str == rhs.str_ptr->u8str; }
	bool operator< (const str_utf8_wrapper &rhs) const noexcept { return this->str_ptr->u8str <  rhs.str_ptr->u8str; }
	bool operator> (const str_utf8_wrapper &rhs) const noexcept { return this->str_ptr->u8str >  rhs.str_ptr->u8str; }
	bool operator<=(const str_utf8_wrapper &rhs) const noexcept { return this->str_ptr->u8str <= rhs.str_ptr->u8str; }
	bool operator>=(const str_utf8_wrapper &rhs) const noexcept { return this->str_ptr->u8str >= rhs.str_ptr->u8str; }
	bool empty() const noexcept { return this->str_ptr->u8str.empty(); }
	const char* c_str() const noexcept { return this->str_ptr->u8str.c_str(); }
	const std::string& toString() const noexcept { return this->str_ptr->u8str; }
	size_t size() const noexcept { return this->str_ptr->u8str.size(); }

	glong get_utf8_strlen() const {
		if (str_ptr->u8len == str_utf8_t::LENGTH_UNKNOWN) {
			str_ptr->u8len = g_utf8_strlen(str_ptr->u8str.c_str(), str_ptr->u8str.size());
		}
		return str_ptr->u8len;
	};

private:
	shared_ptr<str_utf8_t> str_ptr;
};

class FunctionType {
public:
	FunctionType(std::shared_ptr<Context> ctx, std::shared_ptr<Expression> expr, std::shared_ptr<AssignmentList> args)
		: ctx(ctx), expr(expr), args(std::move(args)) { }
	FunctionType(FunctionType&&) = default;
	FunctionType& operator=(FunctionType&&) = default;
	bool operator==(const FunctionType&) const { return false; }
	bool operator!=(const FunctionType& other) const { return !(*this == other); }
	bool operator< (const FunctionType&) const { return false; }
	bool operator> (const FunctionType&) const { return false; }
	bool operator<=(const FunctionType&) const { return false; }
	bool operator>=(const FunctionType&) const { return false; }

	const std::shared_ptr<Context>& getCtx() const { return ctx; }
	const std::shared_ptr<Expression>& getExpr() const { return expr; }
	const AssignmentList& getArgs() const { return *args; }

	friend std::ostream& operator<<(std::ostream& stream, const FunctionType& f);

private:
	std::shared_ptr<Context> ctx;
	std::shared_ptr<Expression> expr;
	std::shared_ptr<AssignmentList> args;
};

using FunctionPtr = ValuePtr<FunctionType>;

/**
 *  Value class encapsulates a boost::variant value which can represent any of the
 *  value types existing in the SCAD language.  
 * -- As part of a refactoring effort which began as PR #2881 and continued as PR #3102, 
 *    Value and its constituent types have been made (nominally) "move only".
 * -- In some cases a copy of a Value is necessary or unavoidable, in which case Value::clone() can be used.
 * -- Value::clone() is used instead of automatic copy construction/assignment so this action is 
 *    made deliberate and explicit (and discouraged).
 * -- Recommended to make use of RVO (Return Value Optimization) wherever possible: 
 * 			https://en.cppreference.com/w/cpp/language/copy_elision
 * -- Classes which cache Values such as Context or dxf_dim_cache(see dxfdim.cc), when queried 
 *    should return either a const reference or a clone of the cached value if returning by-value.
 *    NEVER return a non-const reference!
 */
class Value
{
public:
	enum class Type {
		UNDEFINED,
		BOOL,
		NUMBER,
		STRING,
		VECTOR,
		EMBEDDED_VECTOR,
		RANGE,
		FUNCTION
	};
	static const Value undefined;

	/**
	 * VectorType is the underlying "BoundedType" of boost::variant for OpenSCAD vectors.
	 * It holds only a a shared_ptr to its VectorObject type, and provides a convenient 
	 * interface for various operations needed on its vector.
	 * 
	 * EmbeddedVectorType class derives from VectorType and enables O(1) concatentation of vectors 
	 * by treating their elements as elements of their parent, traversable via VectorType's custom iterator.
	 * -- An embedded vector should never exist "in the wild", only as a pseudo-element of a parent vector.
	 *    Eg "Lc*" Expressions return Embedded Vectors but they are necessairly child expressions of a Vector expression.
	 * -- Any VectorType containing embedded elements will be forced to "flatten" upon usage of operator[], 
	 *    which is the only case of random-access.
	 * -- Any loops through VectorTypes should prefer automatic range-based for loops  eg: for(const auto& value : vec) { ... }
	 *    which make use of begin() and end() iterators of VectorType.  https://en.cppreference.com/w/cpp/language/range-for
	 * -- Moving a temporary Value of type VectorType or EmbeddedVectorType is always safe,
	 *    since it just moves the shared_ptr in its possession (which might be a copy but that doesn't matter).
	 *    Additionally any VectorType can be converted to an EmbeddedVectorType by moving it into 
	 *    EmbeddedVectorType's converting constructor (or vice-versa).  
	 * -- HOWEVER, moving elements out of a [Embedded]VectorType is potentially DANGEROUS unless it can be
	 *    verified that ( ptr.use_count() == 1 ) for that outermost [Embedded]VectorType 
	 *    AND recursively any EmbeddedVectorTypes which led to that element.  
	 *    Therefore elements are currently cloned rather than making any attempt to move.
	 *    Performing such use_count checks may be an area for further optimization.
	 */
	class EmbeddedVectorType;
	class VectorType {

	protected:
		// The object type which VectorType's shared_ptr points to
		struct VectorObject {
			using vec_t = std::vector<Value>;
			using size_type = vec_t::size_type;
			vec_t vec;
			// Keep count of the number of embedded elements *excess of* vec.size()
			size_type embed_excess = 0; 
		};
		using vec_t = VectorObject::vec_t;

		// A Deleter is used on the shared_ptrs to avoid stack overflow in cases 
		// of destructing a very large list of nested embedded vectors, such as from a 
		// recursive function which concats one element at a time.  
		// (A similar solution can also be seen with csgnode.h:CSGOperationDeleter).
		struct VectorObjectDeleter {
			void operator()(VectorObject* vec);
		};

		shared_ptr<VectorObject> ptr;
		void flatten() const; // flatten replaces the VectorObject with a 
		explicit VectorType(const shared_ptr<VectorObject> &copy) : ptr(copy) { } // called by clone()

	public:
		using size_type = VectorObject::size_type;
		static const VectorType EMPTY;

		class iterator {
		public:
			using iterator_category = std::forward_iterator_tag ;
			using value_type        = Value;
			using difference_type   = void;
			using reference         = const value_type&;
			using pointer           = const value_type*;
			iterator() noexcept : it_stack(), it(EMPTY.ptr->vec.begin()), end(EMPTY.ptr->vec.end()) {} // DefaultConstructible
			iterator(const VectorType& vec, bool get_end=false) noexcept;
			iterator& operator++();
			reference operator*() const { return *it; };
			pointer operator->() const { return &*it; };
			bool operator==(const iterator &other) const { return this->it == other.it && this->it_stack == other.it_stack; }
			bool operator!=(const iterator &other) const { return this->it != other.it || this->it_stack != other.it_stack; }
		private:
			inline void check_and_push();
			std::vector<std::pair<vec_t::const_iterator, vec_t::const_iterator> > it_stack;
			vec_t::const_iterator it, end;
		};

		using const_iterator = const iterator;

		VectorType() : ptr(shared_ptr<VectorObject>(new VectorObject(), VectorObjectDeleter() )) {}
		VectorType(double x, double y, double z);
		VectorType(const VectorType &) = delete; // never copy, move instead
		VectorType& operator=(const VectorType &) = delete; // never copy, move instead
		VectorType(VectorType&&) = default; // default move constructor
		VectorType& operator=(VectorType&&) = default; // default move assignment

		// Copy explicitly only when necessary
		VectorType clone() const { return VectorType(this->ptr); }

		// const accesses to VectorObject require .clone to be move-able
		const Value &operator[](size_t idx) const noexcept { 
			if (idx < this->size()) {
				if (ptr->embed_excess) flatten();
	 			return ptr->vec[idx];
			} else {
				return Value::undefined;
			}
		}

		const_iterator begin() const noexcept { return iterator(*this); }
		const_iterator   end() const noexcept { return iterator(*this, true);   }
		size_type size() const { return ptr->vec.size() + ptr->embed_excess;	}
		bool empty() const noexcept { return ptr->vec.empty();	}
		static Value EmptyVector() { return Value(EMPTY.clone()); }

		bool operator==(const VectorType &v) const;
		bool operator!=(const VectorType &v) const;
		bool operator< (const VectorType &v) const;
		bool operator> (const VectorType &v) const;
		bool operator<=(const VectorType &v) const;
		bool operator>=(const VectorType &v) const;

		template<typename... Args> void emplace_back(Args&&... args) { ptr->vec.emplace_back(std::forward<Args>(args)...); }
		void emplace_back(Value&& val);
		void emplace_back(EmbeddedVectorType&& mbed);
	};

	class EmbeddedVectorType : public VectorType {
	private:
			static const EmbeddedVectorType EMPTY;
			explicit EmbeddedVectorType(const shared_ptr<VectorObject> &copy) : VectorType(copy) { } // called by clone()
	public:
		EmbeddedVectorType() : VectorType() {};
		EmbeddedVectorType(EmbeddedVectorType&&) = default;
		EmbeddedVectorType(VectorType&& v) : VectorType(std::move(v)) {}; // converting constructor
		EmbeddedVectorType clone() const { return EmbeddedVectorType(this->ptr); }

		EmbeddedVectorType& operator=(EmbeddedVectorType&&) = default;
		static Value EmptyVector() { return Value(EMPTY.clone()); }
	};

private:
	Value() : value(boost::blank()) { } // Don't default construct empty Values.  If "undefined" needed, use reference to Value::undefined, or call Value::undef() for return by value
public:
	static Value undef() { return Value(); }
	Value(const Value &) = delete; // never copy, move instead
	Value &operator=(const Value &v) = delete; // never copy, move instead
	Value(Value&&) = default; // default move constructor
	Value& operator=(Value&&) = default; // default move assignment

	Value(int v) : value(double(v)) { }
	Value(double d) : value(d) { }
	template<class Variant> Value(Variant&& val) : value(std::forward<Variant>(val)) { }

	Value clone() const; // Use sparingly to explicitly copy a Value

	const std::string typeName() const;
	Type type() const { return static_cast<Type>(this->value.which()); }
	bool isDefinedAs(const Type type) const { return this->type() == type; }
	bool isDefined()   const { return this->type() != Type::UNDEFINED; }
	bool isUndefined() const { return this->type() == Type::UNDEFINED; }

	// Conversion to boost::variant "BoundedType"s. const ref where appropriate.
	bool toBool() const;
	double toDouble() const;
	const str_utf8_wrapper& toStrUtf8Wrapper() const;
	const VectorType &toVector() const;
	const EmbeddedVectorType& toEmbeddedVector() const;
	VectorType &toVectorNonConst();
	EmbeddedVectorType &toEmbeddedVectorNonConst();
	const RangeType& toRange() const;
	const FunctionType& toFunction() const;

	// Other conversion utility functions
	bool getDouble(double &v) const;
	bool getFiniteDouble(double &v) const;
	std::string toString() const;
	std::string toString(const tostring_visitor *visitor) const;
	std::string toEchoString() const;
	std::string toEchoString(const tostring_visitor *visitor) const;
	void toStream(std::ostringstream &stream) const;
	void toStream(const tostream_visitor *visitor) const;
	std::string chrString() const;
	bool getVec2(double &x, double &y, bool ignoreInfinite = false) const;
	bool getVec3(double &x, double &y, double &z) const;
	bool getVec3(double &x, double &y, double &z, double defaultval) const;

	// Common Operators
	explicit operator bool() const { return this->toBool(); } // use explicit to avoid accidental conversion in constructors
	bool operator==(const Value &v) const;
	bool operator!=(const Value &v) const;
	bool operator<(const Value &v) const;
	bool operator<=(const Value &v) const;
	bool operator>=(const Value &v) const;
	bool operator>(const Value &v) const;
	Value operator-() const;
	Value operator[](const Value &v) const;
	Value operator[](size_t idx) const;
	Value operator+(const Value &v) const;
	Value operator-(const Value &v) const;
	Value operator*(const Value &v) const;
	Value operator/(const Value &v) const;
	Value operator%(const Value &v) const;

	friend std::ostream &operator<<(std::ostream &stream, const Value &value) {
		if (value.type() == Value::Type::STRING) stream << QuotedString(value.toString());
		else stream << value.toString();
		return stream;
	}

	typedef boost::variant<boost::blank, bool, double, str_utf8_wrapper, VectorType, EmbeddedVectorType, RangePtr, FunctionPtr> Variant;
	static_assert(sizeof(Variant) <= 24, "Memory size of Value too big");

private:
	Variant value;
};

using VectorType = Value::VectorType;
using EmbeddedVectorType = Value::EmbeddedVectorType;
