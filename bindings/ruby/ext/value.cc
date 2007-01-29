#include "typelib.hh"
extern "C" {
#include <dl.h>
}

#include <sstream>
#include <limits>
#include <typelib/value.hh>
#include <typelib/registry.hh>
#include <typelib/typevisitor.hh>
#include <typelib/csvoutput.hh>
#include <typelib/endianness.hh>

using namespace Typelib;
using std::numeric_limits;

// NOP deleter, for Type objects and some Ptr objects
static void do_not_delete(void*) {}

namespace cxx2rb {
    /* There are constraints when creating a Ruby wrapper for a Type,
     * mainly for avoiding GC issues
     * This function does the work
     * It needs the registry v type is from
     */
    VALUE value_wrap(Value v, VALUE registry, VALUE klass, VALUE dlptr)
    {
        VALUE type      = type_wrap(v.getType(), registry);
	if (NIL_P(dlptr))
	    dlptr = rb_dlptr_new(v.getData(), v.getType().getSize(), do_not_delete);

        return rb_funcall(type, rb_intern("wrap"), 1, dlptr);
    }
}

VALUE cType	 = Qnil;
VALUE cIndirect  = Qnil;
VALUE cPointer   = Qnil;
VALUE cArray     = Qnil;
VALUE cCompound  = Qnil;
VALUE cEnum      = Qnil;

VALUE cxx2rb::class_of(Typelib::Type const& type)
{
    using Typelib::Type;
    switch(type.getCategory()) {
	case Type::Compound:    return cCompound;
	case Type::Pointer:     return cPointer;
	case Type::Array:       return cArray;
	case Type::Enum:        return cEnum;
	default:                return cType;
    }
}

VALUE cxx2rb::type_wrap(Type const& type, VALUE registry)
{
    VALUE known_types = rb_iv_get(registry, "@wrappers");
    if (NIL_P(known_types))
	rb_raise(rb_eArgError, "@wrappers is uninitialized");

    // Type objects are unique, we can register Ruby wrappers based
    // on the type pointer (instead of using names)
    WrapperMap& wrappers = rb2cxx::get_wrapped<WrapperMap>(known_types);

    WrapperMap::const_iterator it = wrappers.find(&type);
    if (it != wrappers.end())
	return it->second;

    VALUE base  = class_of(type);
    VALUE klass = rb_funcall(rb_cClass, rb_intern("new"), 1, base);
    VALUE rb_type = Data_Wrap_Struct(rb_cObject, 0, do_not_delete, const_cast<Type*>(&type));
    rb_iv_set(klass, "@registry", registry);
    rb_iv_set(klass, "@type", rb_type);
    rb_iv_set(klass, "@name", rb_str_new2(type.getName().c_str()));
    rb_iv_set(klass, "@null", (type.getCategory() == Type::NullType) ? Qtrue : Qfalse);

    if (rb_respond_to(klass, rb_intern("subclass_initialize")))
	rb_funcall(klass, rb_intern("subclass_initialize"), 0);

    wrappers.insert(std::make_pair(&type, klass));
    return klass;
}

/**********************************************
 * Typelib::Type
 */

/** 
 * call-seq:
 *   type.to_csv([basename [, separator]])	    => string
 *
 * Returns a one-line representation of this type, using +separator+ 
 * to separate each fields. If +basename+ is given, use it as a 
 * 'variable name'. For instance, calling this method on an array
 * with a basename of 'array' will return
 *  
 *   array[0] array[1]
 *
 * without basename, it would be 
 *
 *   [0] [1]
 *
 */
static VALUE type_to_csv(int argc, VALUE* argv, VALUE rbself)
{
    VALUE basename = Qnil;
    VALUE separator = Qnil;
    rb_scan_args(argc, argv, "02", &basename, &separator);

    std::string bname = "", sep = " ";
    if (!NIL_P(basename)) bname = StringValuePtr(basename);
    if (!NIL_P(separator)) sep = StringValuePtr(separator);

    Type const& self(rb2cxx::object<Type>(rbself));
    std::ostringstream stream;
    stream << csv_header(self, bname, sep);
    std::string str = stream.str();
    return rb_str_new(str.c_str(), str.length());
}

static VALUE type_equal_operator(VALUE rbself, VALUE rbwith)
{ 
    if (! rb_respond_to(rbwith, rb_intern("superclass")))
	return Qfalse;
    if (rb_funcall(rbself, rb_intern("superclass"), 0) != rb_funcall(rbwith, rb_intern("superclass"), 0))
        return Qfalse;

    Type const& self(rb2cxx::object<Type>(rbself));
    Type const& with(rb2cxx::object<Type>(rbwith));
    bool result = (self == with) || self.isSame(with);
    return result ? Qtrue : Qfalse;
}


/* PODs are assignable, pointers are dereferenced */
static VALUE type_is_assignable(Type const& type)
{
    switch(type.getCategory())
    {
    case Type::Numeric:
        return INT2FIX(1);
    case Type::Pointer:
        return type_is_assignable( dynamic_cast<Pointer const&>(type).getIndirection());
    case Type::Enum:
        return INT2FIX(1);
    default:
        return INT2FIX(0);
    }
    // never reached
}


/***********************************************************************************
 *
 * Wrapping of the Value class
 *
 */

static void value_delete(void* self) { delete reinterpret_cast<Value*>(self); }

static VALUE value_alloc(VALUE klass)
{ return Data_Wrap_Struct(klass, 0, value_delete, new Value); }

static
VALUE value_initialize(VALUE self, VALUE ptr)
{
    Value& value = rb2cxx::object<Value>(self);
    Type const& t(rb2cxx::object<Type>(rb_class_of(self)));

    if (NIL_P(ptr) || TYPE(ptr) == T_STRING)
    {
        VALUE buffer = rb_dlptr_malloc(t.getSize(), free);
	if (! NIL_P(ptr))
	    memcpy(rb_dlptr2cptr(buffer), StringValuePtr(ptr), t.getSize());
	ptr = buffer;
    }

    // Protect 'ptr' against the GC
    rb_iv_set(self, "@ptr", ptr);

    value = Value(rb_dlptr2cptr(ptr), t);
    return self;
}

static
VALUE value_endian_swap_b(VALUE self)
{
    Value& value = rb2cxx::object<Value>(self);
    endian_swap(value);
    return self;
}

static
VALUE value_to_byte_array(VALUE self)
{
    Value& value = rb2cxx::object<Value>(self);
    return rb_str_new(reinterpret_cast<const char*>(value.getData()), value.getType().getSize());
}

VALUE value_memory_eql_p(VALUE rbself, VALUE rbwith)
{
    Value& self = rb2cxx::object<Value>(rbself);
    Value& with = rb2cxx::object<Value>(rbwith);
    if (self.getData() == with.getData())
	return Qtrue;
    
    // Type#== checks for type equality before calling memory_equal?
    Type const& type = self.getType();
    return memcmp(self.getData(), with.getData(), type.getSize()) == 0 ? Qtrue : Qfalse;
}

VALUE value_get_registry(VALUE self)
{
    VALUE type = rb_class_of(self);
    return rb_iv_get(type, "@registry");
}

/* 
 * call-seq:
 *   value.to_ruby	=> non-Typelib object or self
 *
 * Converts +self+ to its Ruby equivalent. If no equivalent
 * type is available, returns self
 */
static VALUE value_to_ruby(VALUE self)
{
    Value const& value(rb2cxx::object<Value>(self));
    VALUE registry = value_get_registry(self);
    return typelib_to_ruby(value, registry);
}

/** 
 * call-seq:
 *   value.to_csv([separator])	    => string
 *
 * Returns a one-line representation of this value, using +separator+ 
 * to separate each fields
 */
static VALUE value_to_csv(int argc, VALUE* argv, VALUE self)
{
    VALUE separator = Qnil;
    rb_scan_args(argc, argv, "01", &separator);

    Value const& value(rb2cxx::object<Value>(self));
    std::string sep = " ";
    if (!NIL_P(separator)) sep = StringValuePtr(separator);

    std::ostringstream stream;
    stream << csv(value.getType(), value.getData(), sep);
    std::string str = stream.str();
    return rb_str_new(str.c_str(), str.length());
}

/* Initializes the memory to 0 */
static VALUE value_zero(VALUE self)
{
    Value const& value(rb2cxx::object<Value>(self));
    memset(value.getData(), value.getType().getSize(), 0);
    return self;
}

static void typelib_validate_value_arg(VALUE arg, void*& data, size_t& size)
{
    Value const& value(rb2cxx::object<Value>(arg));
    Type  const& type = value.getType();
    if (type.getCategory() == Type::Pointer)
	size = numeric_limits<size_t>::max();
    else if (type.getCategory() == Type::Array)
	size = static_cast<Array const&>(type).getSize();
    else
	rb_raise(rb_eArgError, "invalid argument for memcpy: only pointers, arrays or strings are allowed");

    data = value.getData();
}
static VALUE typelib_memcpy(VALUE, VALUE to, VALUE from, VALUE size)
{
    void * p_to, * p_from;
    size_t size_to, size_from;
    if (TYPE(to) == T_STRING)
    {
	to = StringValue(to);
	rb_str_modify(to);

	p_to    = RSTRING(to)->ptr;
	size_to = RSTRING(to)->len;
    }
    else typelib_validate_value_arg(to, p_to, size_to);

    if (TYPE(from) == T_STRING)
    {
	p_from = RSTRING(from)->ptr;
	size_from = RSTRING(from)->len;
    }
    else typelib_validate_value_arg(from, p_from, size_from);

    size_t copy_size = NUM2UINT(size);
    if (size_to < copy_size)
	rb_raise(rb_eArgError, "destination buffer too small");
    else if (size_from < copy_size)
	rb_raise(rb_eArgError, "source buffer too small");

    memcpy(p_to, p_from, copy_size);
    return to;
}

void Typelib_init_values()
{
    VALUE mTypelib  = rb_define_module("Typelib");
    rb_define_singleton_method(mTypelib, "memcpy", RUBY_METHOD_FUNC(typelib_memcpy), 3);

    cType     = rb_define_class_under(mTypelib, "Type", rb_cObject);
    rb_define_alloc_func(cType, value_alloc);
    rb_define_singleton_method(cType, "==",	    RUBY_METHOD_FUNC(type_equal_operator), 1);
    rb_define_method(cType, "__initialize__",   RUBY_METHOD_FUNC(&value_initialize), 1);
    rb_define_method(cType, "to_ruby",      RUBY_METHOD_FUNC(&value_to_ruby), 0);
    rb_define_method(cType, "zero!",      RUBY_METHOD_FUNC(&value_zero), 0);
    rb_define_method(cType, "memory_eql?",      RUBY_METHOD_FUNC(&value_memory_eql_p), 1);
    rb_define_method(cType, "endian_swap_b",      RUBY_METHOD_FUNC(&value_endian_swap_b), 1);

    rb_define_singleton_method(cType, "to_csv", RUBY_METHOD_FUNC(type_to_csv), -1);
    rb_define_method(cType, "to_csv", RUBY_METHOD_FUNC(value_to_csv), -1);
    rb_define_method(cType, "to_byte_array", RUBY_METHOD_FUNC(value_to_byte_array), 0);

    Typelib_init_specialized_types();
}

