/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <cmath>
#include <numeric>
#include <iterator>
#include <assert.h>
#include <sstream>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/range/adaptor/transformed.hpp>
/*Unicode support for string lengths and array accesses*/
#include <glib.h>
#include <boost/graph/named_function_params.hpp>

#include "value.h"
#include "expression.h"
#include "printutils.h"
#include "boost-utils.h"
#include "double-conversion/double-conversion.h"
#include "double-conversion/utils.h"
#include "double-conversion/ieee.h"

namespace fs=boost::filesystem;
using boost::adaptors::transformed;
using boost::algorithm::join;

const Value Value::undefined;

/* Define values for double-conversion library. */
#define DC_BUFFER_SIZE 128
#define DC_FLAGS (double_conversion::DoubleToStringConverter::UNIQUE_ZERO | double_conversion::DoubleToStringConverter::EMIT_POSITIVE_EXPONENT_SIGN)
#define DC_INF "inf"
#define DC_NAN "nan"
#define DC_EXP 'e'
#define DC_DECIMAL_LOW_EXP -6
#define DC_DECIMAL_HIGH_EXP 21
#define DC_MAX_LEADING_ZEROES 5
#define DC_MAX_TRAILING_ZEROES 0

/* WARNING: using values > 8 will significantly slow double to string
 * conversion, defeating the purpose of using double-conversion library */
#define DC_PRECISION_REQUESTED 6

//private definitions used by trimTrailingZeroesHelper
#define TRIM_TRAILINGZEROES_DONE 0
#define TRIM_TRAILINGZEROES_CONTINUE 1


//process parameter buffer from the end to start to find out where the zeroes are located (if any).
//parameter pos shall be the pos in buffer where '\0' is located.
//parameter currentpos shall be set to end of buffer (where '\0' is located).
//set parameters exppos and decimalpos when needed.
//leave parameter zeropos as is.
inline int trimTrailingZeroesHelper(char *buffer, const int pos, char *currentpos=nullptr, char *exppos=nullptr, char *decimalpos=nullptr, char *zeropos=nullptr) {
    
    int cont = TRIM_TRAILINGZEROES_CONTINUE;
  
    //we have exhaused all positions from end to start
    if(currentpos <= buffer)
        return TRIM_TRAILINGZEROES_DONE;
    
    //we do no need to process the terminator of string
    if(*currentpos == '\0'){
        currentpos--;
        cont = trimTrailingZeroesHelper(buffer, pos, currentpos, exppos, decimalpos, zeropos);
    }
    
    
    //we have an exponent and jumps to the position before the exponent - no need to process the characters belonging to the exponent
    if(cont && exppos && currentpos >= exppos)
    {
        currentpos = exppos;
        currentpos--;
        cont = trimTrailingZeroesHelper(buffer, pos, currentpos , exppos, decimalpos, zeropos);
    }
    
    //we are still on the right side of the decimal and still counting zeroes (keep track of) from the back to start
    if(cont && currentpos && decimalpos < currentpos && *currentpos == '0'){
        zeropos= currentpos;
        currentpos--;
        cont = trimTrailingZeroesHelper(buffer, pos, currentpos, exppos, decimalpos, zeropos);
    }
    
    //we have found the first occurrance of not a zero and have zeroes and exponent to take care of (move exponent to either the position of the zero or the decimal)
    if(cont && zeropos && exppos){
        int count = &buffer[pos] - exppos + 1;
        memmove(zeropos - 1 == decimalpos ? decimalpos : zeropos, exppos, count);
        return TRIM_TRAILINGZEROES_DONE;
    }
    
    //we have found a zero and need to take care of (truncate the string to the position of either the zero or the decimal)
    if(cont && zeropos){
       zeropos - 1 == decimalpos ? *decimalpos = '\0' : *zeropos = '\0';
       return TRIM_TRAILINGZEROES_DONE;
    }
    
    //we have just another character (other than a zero) and are done
    if(cont && !zeropos)
       return TRIM_TRAILINGZEROES_DONE;

    return TRIM_TRAILINGZEROES_DONE;
}

inline void trimTrailingZeroes(char *buffer, const int pos) {
  char *decimal = strchr(buffer, '.');
  if (decimal){ 
      char *exppos = strchr(buffer, DC_EXP);
      trimTrailingZeroesHelper(buffer, pos, &buffer[pos], exppos, decimal, nullptr);
  }
}

inline bool HandleSpecialValues(const double &value, double_conversion::StringBuilder &builder) {
  double_conversion::Double double_inspect(value);
  if (double_inspect.IsInfinite()) {
    if (value < 0) {
      builder.AddCharacter('-');
    }
    builder.AddString(DC_INF);
    return true;
  }
  if (double_inspect.IsNan()) {
    builder.AddString(DC_NAN);
    return true;
  }
  return false;
}

inline std::string DoubleConvert(const double &value, char *buffer,
    double_conversion::StringBuilder &builder, const double_conversion::DoubleToStringConverter &dc) {
  builder.Reset();
  if (double_conversion::Double(value).IsSpecial()) {
    HandleSpecialValues(value, builder);
    builder.Finalize();
    return buffer;
  }
  dc.ToPrecision(value, DC_PRECISION_REQUESTED, &builder);
  int pos = builder.position(); // get position before Finalize destroys it
  builder.Finalize();
  trimTrailingZeroes(buffer, pos);
  return buffer;
}

void utf8_split(const str_utf8_wrapper& str, std::function<void(Value)> f)
{
    const char *ptr = str.c_str();

    while (*ptr) {
        auto next = g_utf8_next_char(ptr);
        const size_t length = next - ptr;
        f(Value(std::string{ptr, length}));
        ptr = next;
    }
}

static uint32_t convert_to_uint32(const double d)
{
	auto ret = std::numeric_limits<uint32_t>::max();

	if (std::isfinite(d)) {
		try {
			ret = boost::numeric_cast<uint32_t>(d);
		} catch (boost::bad_numeric_cast &) {
			// ignore, leaving the default max() value
		}
	}

	return ret;
}

std::ostream &operator<<(std::ostream &stream, const Filename &filename)
{
  fs::path fnpath{static_cast<std::string>(filename)}; // gcc-4.6
	auto fpath = boostfs_uncomplete(fnpath, fs::current_path());
  stream << QuotedString(fpath.generic_string());
  return stream;
}

// FIXME: This could probably be done more elegantly using boost::regex
std::ostream &operator<<(std::ostream &stream, const QuotedString &s)
{
  stream << '"';
  for (char c : s) {
    switch (c) {
    case '\t':
      stream << "\\t";
      break;
    case '\n':
      stream << "\\n";
      break;
    case '\r':
      stream << "\\r";
      break;
    case '"':
    case '\\':
      stream << '\\';
      stream << c;
      break;
    default:
      stream << c;
    }
  }
  stream << '"';
  return stream;
}

Value Value::clone() const {
  Value c;
  switch (this->type()) {
    case ValueType::UNDEFINED :
    break;
    case ValueType::BOOL :
      c.value = boost::get<bool>(this->value);
    break;
    case ValueType::NUMBER :
      c.value = boost::get<double>(this->value);
    break;
    case ValueType::STRING :
      c.value = boost::get<str_utf8_wrapper>(this->value).clone();
    break;
    case ValueType::RANGE :
      c.value = boost::get<RangePtr>(this->value);
    break;
    case ValueType::VECTOR :
      c.value = boost::get<VectorPtr>(this->value).clone();
    break;
    case ValueType::FUNCTION :
      c.value = boost::get<FunctionPtr>(this->value);
    break;
    default:
      assert(false && "unknown Variant value");
  }
  return c;
}

const std::string Value::typeName() const
{
	switch (this->type()) {
	case ValueType::UNDEFINED:
		return "undefined";
	case ValueType::BOOL:
		return "bool";
	case ValueType::NUMBER:
		return "number";
	case ValueType::STRING:
		return "string";
	case ValueType::VECTOR:
		return "vector";
	case ValueType::RANGE:
		return "range";
	case ValueType::FUNCTION:
		return "function";
	default:
		return "<unknown>";
	}
}

bool Value::toBool() const
{
  switch (this->type()) {
  case ValueType::BOOL:
    return boost::get<bool>(this->value);
    break;
  case ValueType::NUMBER:
    return boost::get<double>(this->value)!= 0;
    break;
  case ValueType::STRING:
    return !boost::get<str_utf8_wrapper>(this->value).empty();
    break;
  case ValueType::VECTOR:
    return !boost::get<VectorPtr>(this->value)->empty();
    break;
  case ValueType::RANGE:
    return true;
    break;
  default:
    return false;
    break;
  }
}

double Value::toDouble() const
{
  const double *d = boost::get<double>(&this->value);
  return d ? *d : 0.0;
}

bool Value::getDouble(double &v) const
{
  const double *d = boost::get<double>(&this->value);
  if (d) {
    v = *d;
    return true;
  }
  return false;
}

bool Value::getFiniteDouble(double &v) const
{
  double result;
  if (!getDouble(result)) {
    return false;
  }
  bool valid = std::isfinite(result);
  if (valid) {
    v = result;
  }
  return valid;
}

Value::VectorPtr::VectorPtr(double x, double y, double z) : ptr(make_shared<Value::VectorType>()) {
  (*this)->emplace_back(x);
  (*this)->emplace_back(y);
  (*this)->emplace_back(z);
}

const str_utf8_wrapper& Value::toStrUtf8Wrapper() const {
  static const str_utf8_wrapper empty{};
  const str_utf8_wrapper *s = boost::get<str_utf8_wrapper>(&this->value);
  return s ? *s : empty;
}

class tostring_visitor : public boost::static_visitor<std::string>
{
public:
	typedef std::string result_type;
  template <typename T> std::string operator()(const T &op1) const {
    assert(false && "unhandled tostring_visitor type");
    return boost::lexical_cast<std::string>(op1);	
  }

  std::string operator()(const str_utf8_wrapper &op1) const {
    return op1.toString();
  }

  std::string operator()(const double &op1) const {
    char buffer[DC_BUFFER_SIZE];
    double_conversion::StringBuilder builder(buffer, DC_BUFFER_SIZE);
    double_conversion::DoubleToStringConverter dc(DC_FLAGS, DC_INF, DC_NAN, DC_EXP,
      DC_DECIMAL_LOW_EXP, DC_DECIMAL_HIGH_EXP, DC_MAX_LEADING_ZEROES, DC_MAX_TRAILING_ZEROES);
    return DoubleConvert(op1, buffer, builder, dc);
  }

  std::string operator()(const boost::blank &) const {
    return "undef";
  }

  std::string operator()(const bool &v) const {
    return v ? "true" : "false";
  }

  std::string operator()(const Value::VectorPtr &v) const {
    // Create a single stream and pass reference to it for list elements for optimization.
    std::ostringstream stream;
    stream << '[';
    for (size_t i = 0; i < v->size(); i++) {
      if (i > 0) stream << ", ";
      v[i].toStream(stream);
    }
    stream << ']';
    return stream.str();
  }

  std::string operator()(const RangePtr &v) const {
	return STR(*v);
  }

  std::string operator()(const FunctionPtr &v) const {
	return STR(*v);
  }
};

// Optimization to avoid multiple stream instantiations and copies to str for long vectors.
// Functions identically to "class tostring_visitor", except outputting to stream and not returning strings
class tostream_visitor : public boost::static_visitor<>
{
public:
  std::ostringstream &stream;

  mutable char buffer[DC_BUFFER_SIZE];
  mutable double_conversion::StringBuilder builder;
  double_conversion::DoubleToStringConverter dc;

  tostream_visitor(std::ostringstream& stream)
    : stream(stream), builder(buffer, DC_BUFFER_SIZE),
      dc(DC_FLAGS, DC_INF, DC_NAN, DC_EXP, DC_DECIMAL_LOW_EXP, DC_DECIMAL_HIGH_EXP, DC_MAX_LEADING_ZEROES, DC_MAX_TRAILING_ZEROES)
    {};

  template <typename T> void operator()(const T &op1) const {
    //    std::cout << "[generic tostream_visitor]\n";
    stream << boost::lexical_cast<std::string>(op1);
  }

  void operator()(const double &op1) const {
    stream << DoubleConvert(op1, buffer, builder, dc);
  }

  void operator()(const boost::blank &) const {
    stream << "undef";
  }

  void operator()(const bool &v) const {
    stream << (v ? "true" : "false");
  }

  void operator()(const Value::VectorPtr &v) const {
    stream << '[';
    for (size_t i = 0; i < v->size(); i++) {
      if (i > 0) stream << ", ";
      v[i].toStream(this);
    }
    stream << ']';
  }

  void operator()(const str_utf8_wrapper &v) const {
    stream << '"' << v.toString() << '"';
  }

  void operator()(const RangePtr &v) const {
	stream << *v;
  }

  void operator()(const FunctionPtr &v) const {
	stream << *v;
  }
};

std::string Value::toString() const
{
  return boost::apply_visitor(tostring_visitor(), this->value);
}

// helper called by tostring_visitor methods to avoid extra instantiations
std::string Value::toString(const tostring_visitor *visitor) const
{
  return boost::apply_visitor(*visitor, this->value);
}

void Value::toStream(std::ostringstream &stream) const
{
  boost::apply_visitor(tostream_visitor(stream), this->value);
}

// helper called by tostream_visitor methods to avoid extra instantiations
void Value::toStream(const tostream_visitor *visitor) const
{
  boost::apply_visitor(*visitor, this->value);
}

std::string Value::toEchoString() const
{
	if (type() == Value::ValueType::STRING) {
		return std::string("\"") + toString() + '"';
	} else {
		return toString();
	}
}

// helper called by tostring_visitor methods to avoid extra instantiations
std::string Value::toEchoString(const tostring_visitor *visitor) const
{
	if (type() == Value::ValueType::STRING) {
		return std::string("\"") + toString(visitor) + '"';
	} else {
		return toString(visitor);
	}
}

class chr_visitor : public boost::static_visitor<std::string>
{
public:
	typedef std::string result_type;
	template <typename S> std::string operator()(const S &) const
		{
			return "";
		}

	std::string operator()(const double &v) const
		{
			char buf[8];
			memset(buf, 0, 8);
			if (v > 0) {
				const gunichar c = v;
				if (g_unichar_validate(c) && (c != 0)) {
			    g_unichar_to_utf8(c, buf);
				}
			}
			return std::string(buf);
		}

	std::string operator()(const Value::VectorPtr &v) const
		{
			std::ostringstream stream;
			for (size_t i = 0; i < v->size(); i++) {
				stream << v[i].chrString();
			}
			return stream.str();
		}

	std::string operator()(const RangePtr &v) const
		{
			const uint32_t steps = v->numValues();
			if (steps >= RangeType::MAX_RANGE_STEPS) {
				PRINTB("WARNING: Bad range parameter in for statement: too many elements (%lu).", steps);
				return "";
			}

			std::ostringstream stream;
			for (RangeType::iterator it = v->begin();it != v->end();it++) {
				const Value value(*it);
				stream << value.chrString();
			}
			return stream.str();
		}
};

std::string Value::chrString() const
{
  return boost::apply_visitor(chr_visitor(), this->value);
}

void Value::VectorPtr::append_vector(Value v) {
  assert(v.type() == ValueType::VECTOR);
  VectorPtr &vec = v.toVectorPtr();
  if ((*this)->empty()) {
    ptr = std::move(vec.ptr);
  } else {
    ptr->reserve(ptr->size() + vec->size());
    std::move(std::begin(*vec), std::end(*vec), std::back_inserter(*ptr));
  }
}

void Value::VectorPtr::flatten()
{
	const auto size = std::accumulate(std::begin(**this), std::end(**this), 0, [](int i, const Value &v){
		return i + (v.type() == Value::ValueType::VECTOR ? v.toVector().size() : 1);
	});

	Value::VectorPtr ret;
	ret->reserve(size);
	for (auto& v : **this) {
		if (v.type() == Value::ValueType::VECTOR) {
			ret->insert(std::end(*ret),
					std::make_move_iterator(std::begin(*v.toVectorPtr())),
					std::make_move_iterator(std::end(*v.toVectorPtr())));
		} else {
			ret->emplace_back(std::move(v));
		}
	}
	this->ptr = ret.ptr;
}

const Value::VectorType &Value::toVector() const
{
  static const VectorType empty;
  const Value::VectorPtr *v = boost::get<Value::VectorPtr>(&this->value);
  return v ? **v : empty;
}

bool Value::getVec2(double &x, double &y, bool ignoreInfinite) const
{
  if (this->type() != ValueType::VECTOR) return false;

  const auto &v = this->toVector();

  if (v.size() != 2) return false;

  double rx, ry;
  bool valid = ignoreInfinite
	  ? v[0].getFiniteDouble(rx) && v[1].getFiniteDouble(ry)
	  : v[0].getDouble(rx) && v[1].getDouble(ry);

  if (valid) {
    x = rx;
    y = ry;
  }

  return valid;
}

bool Value::getVec3(double &x, double &y, double &z) const
{
  if (this->type() != ValueType::VECTOR) return false;

  const VectorType &v = this->toVector();

  if (v.size() != 3) return false;

  return (v[0].getDouble(x) && v[1].getDouble(y) && v[2].getDouble(z));
}

bool Value::getVec3(double &x, double &y, double &z, double defaultval) const
{
  if (this->type() != ValueType::VECTOR) return false;

  const VectorType &v = toVector();

  if (v.size() == 2) {
    getVec2(x, y);
    z = defaultval;
    return true;
  }
  else {
    if (v.size() != 3) return false;
  }

  return (v[0].getDouble(x) && v[1].getDouble(y) && v[2].getDouble(z));
}

const RangeType& Value::toRange() const
{
	static const RangeType empty{0, 0, 0};
	if (type() == Value::ValueType::RANGE) {
		return *boost::get<RangePtr>(this->value);
	}
	return empty;
}

const FunctionType& Value::toFunction() const
{
	return *boost::get<FunctionPtr>(this->value);
}

class equals_visitor : public boost::static_visitor<bool>
{
public:
	typedef bool result_type;
  template <typename T, typename U> bool operator()(const T &, const U &) const {
    return false;
  }

  template <typename T> bool operator()(const T &op1, const T &op2) const {
    return op1 == op2;
  }

  bool operator()(const bool &op1, const double &op2) const {
    return op1 == op2;
  }

  bool operator()(const double &op1, const bool &op2) const {
    return op1 == op2;
  }
};

bool Value::operator==(const Value &v) const
{
  return boost::apply_visitor(equals_visitor(), this->value, v.value);
}

bool Value::operator!=(const Value &v) const
{
  return !(*this == v);
}

#define DEFINE_VISITOR(name,op)																					\
	class name : public boost::static_visitor<bool>												\
	{																																			\
	public:																																\
		typedef bool result_type;																						\
		template <typename T, typename U> bool operator()(const T &, const U &) const {	\
			return false;																											\
		}																																		\
																																				\
		template <typename T> bool operator()(const T &op1, const T &op2) const {	\
			return op1 op op2;																								\
		}																																		\
																																				\
		bool operator()(const bool &op1, const double &op2) const {					\
			return op1 op op2;																								\
		}																																		\
																																				\
		bool operator()(const double &op1, const bool &op2) const {					\
			return op1 op op2;																								\
		}																																		\
	}

DEFINE_VISITOR(less_visitor, <);
DEFINE_VISITOR(greater_visitor, >);
DEFINE_VISITOR(lessequal_visitor, <=);
DEFINE_VISITOR(greaterequal_visitor, >=);

bool Value::operator<(const Value &v) const
{
	return boost::apply_visitor(less_visitor(), this->value, v.value);
}

bool Value::operator>=(const Value &v) const
{
	return boost::apply_visitor(greaterequal_visitor(), this->value, v.value);
}

bool Value::operator>(const Value &v) const
{
	return boost::apply_visitor(greater_visitor(), this->value, v.value);
}

bool Value::operator<=(const Value &v) const
{
	return boost::apply_visitor(lessequal_visitor(), this->value, v.value);
}

class plus_visitor : public boost::static_visitor<Value>
{
public:
	typedef Value result_type;
	template <typename T, typename U>
  Value operator()(const T &, const U &) const {
		return Value();
	}

	Value operator()(const double &op1, const double &op2) const {
		return Value(op1 + op2);
	}

	Value operator()(const Value::VectorPtr &op1, const Value::VectorPtr &op2) const {
		Value::VectorPtr vec_sum;
		for (size_t i = 0; i < op1->size() && i < op2->size(); i++) {
			vec_sum->emplace_back(op1[i] + op2[i]);
		}
		return vec_sum;
	}
};

Value Value::operator+(const Value &v) const
{
	return boost::apply_visitor(plus_visitor(), this->value, v.value);
}

class minus_visitor : public boost::static_visitor<Value>
{
public:
	typedef Value result_type;
	template <typename T, typename U> Value operator()(const T &, const U &) const {
		return Value();
	}

	Value operator()(const double &op1, const double &op2) const {
		return Value(op1 - op2);
	}

	Value operator()(const Value::VectorPtr &op1, const Value::VectorPtr &op2) const {
		Value::VectorPtr sum;
		for (size_t i = 0; i < op1->size() && i < op2->size(); i++) {
			sum->push_back(op1[i] - op2[i]);
		}
		return sum;
	}
};

Value Value::operator-(const Value &v) const
{
	return boost::apply_visitor(minus_visitor(), this->value, v.value);
}

Value multvecnum(const Value::VectorType &vecval, const Value &numval)
{
// Vector * Number
	Value::VectorPtr dstv;
	for(const auto &val : vecval) {
		dstv->push_back(val * numval);
	}
	return dstv;
}

Value multmatvec(const Value::VectorType &matrixvec, const Value::VectorType &vectorvec)
{
// Matrix * Vector
	Value::VectorPtr dstv;
	for (size_t i=0;i<matrixvec.size();i++) {

		if (matrixvec[i].type() != Value::ValueType::VECTOR ||
				matrixvec[i].toVector().size() != vectorvec.size()) {
			return Value();
		}
		double r_e = 0.0;
		for (size_t j=0;j<matrixvec[i].toVector().size();j++) {
			if (matrixvec[i].toVector()[j].type() != Value::ValueType::NUMBER ||
			    vectorvec[j].type() != Value::ValueType::NUMBER) {
				return Value();
			}
			r_e += matrixvec[i].toVector()[j].toDouble() * vectorvec[j].toDouble();
		}
		dstv->push_back(Value(r_e));
	}
	return dstv;
}

Value multvecmat(const Value::VectorType &vectorvec, const Value::VectorType &matrixvec)
{
	assert(vectorvec.size() == matrixvec.size());
	Value::VectorPtr dstv;
	size_t firstRowSize = matrixvec[0].toVector().size();
	for (size_t i=0;i<firstRowSize;i++) {
		double r_e = 0.0;
		for (size_t j=0;j<vectorvec.size();j++) {
			if (matrixvec[j].type() != Value::ValueType::VECTOR ||
					matrixvec[j].toVector().size() != firstRowSize) {
				PRINTB("WARNING: Matrix must be rectangular. Problem at row %lu", j);
				return Value();
			}
			if (vectorvec[j].type() != Value::ValueType::NUMBER) {
				PRINTB("WARNING: Vector must contain only numbers. Problem at index %lu", j);
				return Value();
			}
			if (matrixvec[j].toVector()[i].type() != Value::ValueType::NUMBER) {
				PRINTB("WARNING: Matrix must contain only numbers. Problem at row %lu, col %lu", j % i);
				return Value();
			}
			r_e += vectorvec[j].toDouble() * matrixvec[j].toVector()[i].toDouble();
		}
		dstv->emplace_back(r_e);
	}
	return dstv;
}

Value multvecvec(const Value::VectorType &vec1, const Value::VectorType &vec2) {
  // Vector dot product.
  auto r = 0.0;
  for (size_t i=0;i<vec1.size();i++) {
    if (vec1[i].type() != Value::ValueType::NUMBER || vec2[i].type() != Value::ValueType::NUMBER) {
      return Value();
    }
    r += vec1[i].toDouble() * vec2[i].toDouble();
  }
  return Value(r);
}

Value Value::operator*(const Value &v) const
{
  if (this->type() == ValueType::NUMBER) {
    if (v.type() == ValueType::NUMBER) {
      return Value(this->toDouble() * v.toDouble());
    } else if (v.type() == ValueType::VECTOR) {
		  return multvecnum(v.toVector(), *this);
    }
  } else if (this->type() == ValueType::VECTOR) {
    if (v.type() == ValueType::NUMBER) {
		  return multvecnum(this->toVector(), v);
    } else if (v.type() == ValueType::VECTOR) {
      const auto &vec1 = this->toVector();
      const auto &vec2 = v.toVector();
      if (vec1.empty() || vec2.empty()) return Value();
      if (vec1[0].type() == ValueType::NUMBER) {
        if (vec2[0].type() == ValueType::NUMBER) {
          if (vec1.size() == vec2.size()) return multvecvec(vec1,vec2);
        } else if (vec2[0].type() == ValueType::VECTOR) {
          if (vec1.size() == vec2.size()) return multvecmat(vec1, vec2);
        }
      } else if (vec1[0].type() == ValueType::VECTOR) {
        if (vec2[0].type() == ValueType::NUMBER) {
          if (vec1[0].toVector().size() == vec2.size()) return multmatvec(vec1, vec2);
        } else if (vec2[0].type() == ValueType::VECTOR) {
          if (vec1[0].toVector().size() == vec2.size()) {
            // Matrix * Matrix
            VectorPtr dstv;
            for (const auto &srcrow : vec1) {
              const auto &srcrowvec = srcrow.toVector();
              if (srcrowvec.size() != vec2.size()) return Value();
              dstv->push_back(multvecmat(srcrowvec, vec2));
            }
            return dstv;
          }
        }
      }
    }
  }
	return Value();
}

Value Value::operator/(const Value &v) const
{
  if (this->type() == ValueType::NUMBER && v.type() == ValueType::NUMBER) {
    return Value(this->toDouble() / v.toDouble());
  }
  else if (this->type() == ValueType::VECTOR && v.type() == ValueType::NUMBER) {
    VectorPtr dstv;
    for (const auto &vecval : this->toVector()) {
      dstv->push_back(vecval / v);
    }
    return dstv;
  }
  else if (this->type() == ValueType::NUMBER && v.type() == ValueType::VECTOR) {
    VectorPtr dstv;
    for (const auto &vecval : v.toVector()) {
      dstv->push_back(*this / vecval);
    }
    return dstv;
  }
  return Value();
}

Value Value::operator%(const Value &v) const
{
  if (this->type() == ValueType::NUMBER && v.type() == ValueType::NUMBER) {
    return Value(fmod(boost::get<double>(this->value), boost::get<double>(v.value)));
  }
  return Value();
}

Value Value::operator-() const
{
  if (this->type() == ValueType::NUMBER) {
    return Value(-this->toDouble());
  }
  else if (this->type() == ValueType::VECTOR) {
    VectorPtr dstv;
    for (const auto &vecval : this->toVector()) {
      dstv->push_back(-vecval);
    }
    return dstv;
  }
  return Value();
}

/*
 * bracket operation [] detecting multi-byte unicode.
 * If the string is multi-byte unicode then the index will offset to the character (2 or 4 byte) and not to the byte.
 * A 'normal' string with byte chars are a subset of unicode and still work.
 */
class bracket_visitor : public boost::static_visitor<Value>
{
public:
	typedef Value result_type;
  Value operator()(const str_utf8_wrapper &str, const double &idx) const {

    const auto i = convert_to_uint32(idx);
    if (i < str.size()) {
			// Ensure character (not byte) index is inside the character/glyph array
			if (i < str.get_utf8_strlen())	{
				gchar utf8_of_cp[6] = ""; //A buffer for a single unicode character to be copied into
				auto ptr = g_utf8_offset_to_pointer(str.c_str(), i);
				if (ptr) {
					g_utf8_strncpy(utf8_of_cp, ptr, 1);
				}
				return Value(std::string(utf8_of_cp));
			}
    }
    return Value();
  }

  Value operator()(const Value::VectorPtr &vec, const double &idx) const {
    const auto i = convert_to_uint32(idx);
    if (i < vec->size()) return vec[i].clone();
    return Value();
  }

  Value operator()(const RangePtr &range, const double &idx) const {
    const auto i = convert_to_uint32(idx);
    switch(i) {
    case 0: return Value(range->begin_val);
    case 1: return Value(range->step_val);
    case 2: return Value(range->end_val);
    }
    return Value();
  }

  template <typename T, typename U> Value operator()(const T &, const U &) const {
    //    std::cout << "generic bracket_visitor\n";
    return Value();
  }
};

Value Value::operator[](const Value &v) const
{
  return boost::apply_visitor(bracket_visitor(), this->value, v.value);
}

Value Value::operator[](size_t idx) const
{
  Value v{(double)idx};
  return boost::apply_visitor(bracket_visitor(), this->value, v.value);
}

uint32_t RangeType::numValues() const
{

	if (std::isnan(begin_val) || std::isnan(end_val) || std::isnan(step_val)) {
		return 0;
	}

	if (step_val < 0) {
		if (begin_val < end_val) {
			return 0;
		}
	} else {
		if (begin_val > end_val) {
		return 0;
		}
	}

	if ((begin_val == end_val) || std::isinf(step_val)) {
		return 1;
	}

	if (std::isinf(begin_val) || std::isinf(end_val) || step_val == 0) {
		return std::numeric_limits<uint32_t>::max();
	}
 
	// Use nextafter to compensate for possible floating point inaccurary where result is just below a whole number.
	const uint32_t max = std::numeric_limits<uint32_t>::max();
	uint32_t num_steps = std::nextafter((end_val - begin_val) / step_val, max);
	return (num_steps == max) ? max : num_steps + 1;
}

RangeType::iterator::iterator(const RangeType &range, type_t type) : range(range), val(range.begin_val), type(type), 
		num_values(range.numValues()), i_step(type == type_t::RANGE_TYPE_END ? num_values : 0)
{
	update_type();
}

void RangeType::iterator::update_type()
{
	if (range.step_val == 0) {
		type = type_t::RANGE_TYPE_END;
	} else if (range.step_val < 0) {
		if (i_step >= num_values) {
			type = type_t::RANGE_TYPE_END;
		}
	} else {
		if (i_step >= num_values) {
			type = type_t::RANGE_TYPE_END;
		}
	}

	if (std::isnan(range.begin_val) || std::isnan(range.end_val) || std::isnan(range.step_val)) {
		type = type_t::RANGE_TYPE_END;
		i_step = num_values;
	}
}

RangeType::iterator::reference RangeType::iterator::operator*()
{
	return val;
}

RangeType::iterator::pointer RangeType::iterator::operator->()
{
	return &(operator*());
}

RangeType::iterator::self_type RangeType::iterator::operator++()
{
	val = range.begin_val + range.step_val * ++i_step;
	update_type();
	return *this;
}

RangeType::iterator::self_type RangeType::iterator::operator++(int)
{
	self_type tmp(*this);
	operator++();
	return tmp;
}

bool RangeType::iterator::operator==(const self_type &other) const
{
	if (type == type_t::RANGE_TYPE_RUNNING) {
		return (type == other.type) && (val == other.val) && (range == other.range);
	} else {
		return (type == other.type) && (range == other.range);
	}
}

bool RangeType::iterator::operator!=(const self_type &other) const
{
	return !(*this == other);
}

std::ostream& operator<<(std::ostream& stream, const RangeType& r)
{
	char buffer[DC_BUFFER_SIZE];
	double_conversion::StringBuilder builder(buffer, DC_BUFFER_SIZE);
	double_conversion::DoubleToStringConverter dc(DC_FLAGS, DC_INF,
			DC_NAN, DC_EXP, DC_DECIMAL_LOW_EXP, DC_DECIMAL_HIGH_EXP,
			DC_MAX_LEADING_ZEROES, DC_MAX_TRAILING_ZEROES);
	stream << "[" << DoubleConvert(r.begin_val, buffer, builder, dc) << " : "
	              << DoubleConvert(r.step_val,  buffer, builder, dc) << " : "
	              << DoubleConvert(r.end_val,   buffer, builder, dc) << "]";
	return stream;
}

std::ostream& operator<<(std::ostream& stream, const FunctionType& f) {
	stream << "function(";
	bool first = true;
	for (const auto& arg : f.getArgs()) {
		stream << (first ? "" : ", ") << arg->name;
		if (arg->expr) {
			stream << " = " << *arg->expr;
		}
		first = false;
	}
	stream << ") " << *f.getExpr();
	return stream;
}
