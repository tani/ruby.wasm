#include <stdlib.h>

#include "ruby.h"

#include "bindgen/rb-js-abi-host.h"

// MARK: - Ruby extension

#ifndef RBOOL
#define RBOOL(v) ((v) ? Qtrue : Qfalse)
#endif

extern VALUE rb_mKernel;

static VALUE rb_mJS;
static VALUE rb_mJS_Object;

static ID i_to_js;

struct jsvalue {
  rb_js_abi_host_js_value_t abi;
};

static void jsvalue_mark(void *p) {}

static void jsvalue_free(void *p) {
  struct jsvalue *ptr = p;
  rb_js_abi_host_js_value_free(&ptr->abi);
  ruby_xfree(ptr);
}

static size_t jsvalue_memsize(const void *p) { return sizeof(struct jsvalue); }

static const rb_data_type_t jsvalue_data_type = {"jsvalue",
                                                 {
                                                     jsvalue_mark,
                                                     jsvalue_free,
                                                     jsvalue_memsize,
                                                 },
                                                 0,
                                                 0,
                                                 RUBY_TYPED_FREE_IMMEDIATELY};
static VALUE jsvalue_s_allocate(VALUE klass) {
  struct jsvalue *p;
  VALUE obj =
      TypedData_Make_Struct(klass, struct jsvalue, &jsvalue_data_type, p);
  return obj;
}

static VALUE jsvalue_s_new(rb_js_abi_host_js_value_t abi) {
  struct jsvalue *p;
  VALUE obj = TypedData_Make_Struct(rb_mJS_Object, struct jsvalue,
                                    &jsvalue_data_type, p);
  p->abi = abi;
  return obj;
}

static struct jsvalue *check_jsvalue(VALUE obj) {
  return rb_check_typeddata(obj, &jsvalue_data_type);
}

#define IS_JSVALUE(obj) (rb_typeddata_is_kind_of((obj), &jsvalue_data_type))

/*
 * call-seq:
 *   JS.eval(code) -> JS::Object
 *
 *  Evaluates the given JavaScript code, returning the result as a JS::Object.
 *
 *   p JS.eval("return 1 + 1").to_i                             # => 2
 *   p JS.eval("return new Object()").is_a?(JS.global[:Object]) # => true
 */
static VALUE _rb_js_eval_js(VALUE _, VALUE code_str) {
  const char *code_str_ptr = (const char *)RSTRING_PTR(code_str);
  rb_js_abi_host_string_t abi_str;
  rb_js_abi_host_string_set(&abi_str, code_str_ptr);
  return jsvalue_s_new(rb_js_abi_host_eval_js(&abi_str));
}

static VALUE _rb_js_is_js(VALUE _, VALUE obj) {
  if (!IS_JSVALUE(obj)) {
    return Qfalse;
  }
  struct jsvalue *val = DATA_PTR(obj);
  return RBOOL(rb_js_abi_host_is_js(val->abi));
}

/*
 * call-seq:
 *   JS.try_convert(obj) -> JS::Object or nil
 *
 *  Try to convert the given object to a JS::Object using <code>to_js</code> method.
 *  Returns <code>nil</code> if the object cannot be converted.
 *
 *   p JS.try_convert(1)          # => 1
 *   p JS.try_convert("foo")      # => "foo"
 *   p JS.try_convert(Object.new) # => nil
 */
VALUE _rb_js_try_convert(VALUE klass, VALUE obj) {
  if (_rb_js_is_js(klass, obj)) {
    return obj;
  } else if (rb_respond_to(obj, i_to_js)) {
    return rb_funcallv(obj, i_to_js, 0, NULL);
  } else {
    return Qnil;
  }
}

/*
 * call-seq:
 *   js_obj.is_a?(js_class) -> true or false
 *
 *  Returns <code>true</code> if <i>js_class</i> is the instance of
 *  <i>js_obj</i>, otherwise returns <code>false</code>.
 *  Comparison is done using the <code>instanceof</code> in JavaScript.
 *
 *   p JS.global.is_a?(JS.global[:Object]) #=> true
 *   p JS.global.is_a?(Object)             #=> false 
 */
static VALUE _rb_js_is_kind_of(VALUE klass, VALUE obj, VALUE c) {
  if (!IS_JSVALUE(obj)) {
    return Qfalse;
  }
  struct jsvalue *val = DATA_PTR(obj);
  VALUE js_klass_v = _rb_js_try_convert(klass, c);
  struct jsvalue *js_klass = DATA_PTR(js_klass_v);
  return RBOOL(rb_js_abi_host_instance_of(val->abi, js_klass->abi));
}

/*
 * call-seq:
 *   JS.global -> JS::Object
 *
 *  Returns <code>globalThis</code> JavaScript object.
 *
 *   p JS.global
 *   p JS.global[:document]
 */
static VALUE _rb_js_global_this(VALUE _) {
  return jsvalue_s_new(rb_js_abi_host_global_this());
}

/*
 * call-seq:
 *   self[prop] -> JS::Object
 *
 * Returns the value of the property:
 *   JS.global[:Object]
 *   JS.global[:console][:log]
 */
static VALUE _rb_js_obj_aref(VALUE obj, VALUE key) {
  struct jsvalue *p = check_jsvalue(obj);
  key = rb_obj_as_string(key);
  const char *key_cstr = (const char *)RSTRING_PTR(key);
  rb_js_abi_host_string_t key_abi_str;
  rb_js_abi_host_string_dup(&key_abi_str, key_cstr);
  return jsvalue_s_new(rb_js_abi_host_reflect_get(p->abi, &key_abi_str));
}

/*
 * call-seq:
 *   self[prop] = value -> JS::Object
 *
 * Set a property on the object with the given value.
 * Returns the value of the property:
 *   JS.global[:Object][:foo] = "bar"
 *   p JS.global[:console][:foo] # => "bar"
 */
static VALUE _rb_js_obj_aset(VALUE obj, VALUE key, VALUE val) {
  struct jsvalue *p = check_jsvalue(obj);
  struct jsvalue *v = check_jsvalue(val);
  key = rb_obj_as_string(key);
  const char *key_cstr = (const char *)RSTRING_PTR(key);
  rb_js_abi_host_string_t key_abi_str;
  rb_js_abi_host_string_dup(&key_abi_str, key_cstr);
  rb_js_abi_host_reflect_set(p->abi, &key_abi_str, v->abi);
  return val;
}

/*
 * :nodoc: all
 * workaround to transfer js value to js by using wit.
 * wit doesn't allow to communicate a resource to guest and host for now.
 */
static VALUE _rb_js_export_to_js(VALUE obj) {
  struct jsvalue *p = check_jsvalue(obj);
  rb_js_abi_host_take_js_value(p->abi);
  return Qnil;
}

/*
 * JavaScript interoperations module
 */
void Init_js() {
  rb_mJS = rb_define_module("JS");
  rb_define_module_function(rb_mJS, "is_a?", _rb_js_is_kind_of, 2);
  rb_define_module_function(rb_mJS, "try_convert", _rb_js_try_convert, 1);
  rb_define_module_function(rb_mJS, "eval", _rb_js_eval_js, 1);
  rb_define_module_function(rb_mJS, "global", _rb_js_global_this, 0);

  i_to_js = rb_intern("to_js");
  rb_mJS_Object = rb_define_class_under(rb_mJS, "Object", rb_cObject);
  rb_define_alloc_func(rb_mJS_Object, jsvalue_s_allocate);
  rb_define_method(rb_mJS_Object, "[]", _rb_js_obj_aref, 1);
  rb_define_method(rb_mJS_Object, "[]=", _rb_js_obj_aset, 2);
  rb_define_method(rb_mJS_Object, "__export_to_js", _rb_js_export_to_js, 0);
}