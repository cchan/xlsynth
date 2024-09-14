// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_PUBLIC_C_API_H_
#define XLS_PUBLIC_C_API_H_

#include <stddef.h>  // NOLINT(modernize-deprecated-headers)
#include <stdint.h>  // NOLINT(modernize-deprecated-headers)

#ifndef XLS_DLL_EXPORT
#define XLS_DLL_EXPORT __attribute__((visibility("default")))
#endif  // XLS_DLL_EXPORT

// C API that exposes the functionality in various public headers in a way that
// C-based FFI facilities can easily wrap.
//
// Note that StatusOr from C++ is generally translated as:
//      StatusOr<T> MyFunction(...) =>
//      bool MyFunction(..., char** error_out, T* out)
//
// The boolean return value indicates "ok" -- if not ok, the `error_out` value
// will be populated with an error string indicating what went wrong -- the
// string will be owned by the caller and will need to be deallocated in the
// case of error.
//
// Caller-owned C strings are created using C standard library facilities and
// thus should be deallocated via `free`.
//
// **WARNING**: These are *not* meant to be *ABI-stable* -- assume you have to
// re-compile against this header for any given XLS commit.

extern "C" {

// Opaque structs.
struct xls_value;
struct xls_package;
struct xls_function;
struct xls_type;
struct xls_function_type;

void xls_init_xls(const char* usage, int argc, char* argv[]);

XLS_DLL_EXPORT
bool xls_convert_dslx_to_ir(const char* dslx, const char* path,
                            const char* module_name,
                            const char* dslx_stdlib_path,
                            const char* additional_search_paths[],
                            size_t additional_search_paths_count,
                            char** error_out, char** ir_out);

XLS_DLL_EXPORT
bool xls_convert_dslx_path_to_ir(const char* path, const char* dslx_stdlib_path,
                                 const char* additional_search_paths[],
                                 size_t additional_search_paths_count,
                                 char** error_out, char** ir_out);

XLS_DLL_EXPORT
bool xls_optimize_ir(const char* ir, const char* top, char** error_out,
                     char** ir_out);

XLS_DLL_EXPORT
bool xls_mangle_dslx_name(const char* module_name, const char* function_name,
                          char** error_out, char** mangled_out);

// Parses a string that represents a typed XLS value; e.g. `bits[32]:0x42`.
XLS_DLL_EXPORT
bool xls_parse_typed_value(const char* input, char** error_out,
                           struct xls_value** xls_value_out);

// Returns a new token XLS value which the caller must free.
XLS_DLL_EXPORT
struct xls_value* xls_value_make_token();

// Returns a new `bits[1]:1` XLS value which the caller must free.
XLS_DLL_EXPORT
struct xls_value* xls_value_make_true();

// Attempts to extract a "bits" value from the given XLS value -- the resulting
// `bits_out` is owned by the caller and must be freed via `xls_bits_free()` on
// success.
bool xls_value_get_bits(const struct xls_value* value, char** error_out,
                        struct xls_bits** bits_out);

void xls_bits_free(struct xls_bits* bits);

// Returns a new `bits[1]:0` XLS value which the caller must free.
XLS_DLL_EXPORT
struct xls_value* xls_value_make_false();

// Returns a string representation of the given value `v`.
XLS_DLL_EXPORT
bool xls_value_to_string(const struct xls_value* v, char** string_out);

// Returns whether `v` is equal to `w`.
XLS_DLL_EXPORT
bool xls_value_eq(const struct xls_value* v, const struct xls_value* w);

// Note: We define the format preference enum with a fixed width integer type
// for clarity of the exposed ABI.
typedef int32_t xls_format_preference;
enum {
  xls_format_preference_default,
  xls_format_preference_binary,
  xls_format_preference_signed_decimal,
  xls_format_preference_unsigned_decimal,
  xls_format_preference_hex,
  xls_format_preference_plain_binary,
  xls_format_preference_plain_hex,
};

// Returns a format preference enum value from a string specifier; i.e.
// `xls_format_preference_from_string("hex")` returns the value of
// `xls_format_preference_hex` -- this is particularly useful for language
// bindings that don't parse the C headers to determine enumerated values.
XLS_DLL_EXPORT
bool xls_format_preference_from_string(const char* s, char** error_out,
                                       xls_format_preference* result_out);

// Returns the given value `v` converted to a string by way of the given
// `format_preference`.
XLS_DLL_EXPORT
bool xls_value_to_string_format_preference(
    const struct xls_value* v, xls_format_preference format_preference,
    char** error_out, char** result_out);

// Deallocates a value, e.g. one as created by `xls_parse_typed_value`.
XLS_DLL_EXPORT
void xls_value_free(struct xls_value* v);

XLS_DLL_EXPORT
void xls_package_free(struct xls_package* p);

// Frees the given `c_str` -- the C string should have been allocated by the
// XLS library where ownership was passed back to the caller.
//
// `c_str` may be null, in which case this function does nothing.
//
// e.g. `xls_convert_dslx_to_ir` gives back `ir_out` which can be deallocated
// by this function.
//
// This function is primarily useful when the underlying allocator may be
// different between the caller and the XLS library (otherwise the caller could
// just call `free` directly).
XLS_DLL_EXPORT
void xls_c_str_free(char* c_str);

// Returns a string representation of the given IR package `p`.
XLS_DLL_EXPORT
bool xls_package_to_string(const struct xls_package* p, char** string_out);

// Parses IR text to a package.
//
// Note: `filename` may be nullptr.
XLS_DLL_EXPORT
bool xls_parse_ir_package(const char* ir, const char* filename,
                          char** error_out,
                          struct xls_package** xls_package_out);

// Returns a function contained within the given `package`.
//
// Note: the returned function does not need to be freed, it is tied to the
// package's lifetime.
XLS_DLL_EXPORT
bool xls_package_get_function(struct xls_package* package,
                              const char* function_name, char** error_out,
                              struct xls_function** result_out);

// Returns the type of the given value, as owned by the given package.
//
// Note: the returned type does not need to be freed, it is tied to the
// package's lifetime.
XLS_DLL_EXPORT
bool xls_package_get_type_for_value(struct xls_package* package,
                                    struct xls_value* value, char** error_out,
                                    struct xls_type** result_out);

// Returns the string representation of the type.
XLS_DLL_EXPORT
bool xls_type_to_string(struct xls_type* type, char** error_out,
                        char** result_out);

// Returns the type of the given function.
//
// Note: the returned type does not need to be freed, it is tied to the
// package's lifetime.
XLS_DLL_EXPORT
bool xls_function_get_type(struct xls_function* function, char** error_out,
                           struct xls_function_type** xls_fn_type_out);

// Returns the name of the given function `function` -- `string_out` is owned
// by the caller and must be freed.
XLS_DLL_EXPORT
bool xls_function_get_name(struct xls_function* function, char** error_out,
                           char** string_out);

// Returns a string representation of the given `xls_function_type`.
XLS_DLL_EXPORT
bool xls_function_type_to_string(struct xls_function_type* xls_function_type,
                                 char** error_out, char** string_out);

// Interprets the given `function` using the given `args` (an array of size
// `argc`) -- interpretation runs to a function result placed in `result_out`,
// or `error_out` is populated and false is returned in the event of an error.
XLS_DLL_EXPORT
bool xls_interpret_function(struct xls_function* function, size_t argc,
                            const struct xls_value** args, char** error_out,
                            struct xls_value** result_out);

// -- VAST (Verilog AST) APIs
//
// Note that these are expected to be *less* stable than the above APIs, as
// they are exposing a useful implementation library present within XLS.
//
// Per usual, in a general sense, no promises are made around API or ABI
// stability overall. However, seems worth noting these are effectively
// "protected" APIs, use with particular caution around stability. See
// `xls/protected/BUILD` for how we tend to think about "protected" APIs in the
// project.

// Opaque structs.
struct xls_vast_verilog_file;
struct xls_vast_verilog_module;
struct xls_vast_node;
struct xls_vast_expression;
struct xls_vast_logic_ref;
struct xls_vast_data_type;
struct xls_vast_indexable_expression;
struct xls_vast_slice;
struct xls_vast_literal;
struct xls_vast_instantiation;
struct xls_vast_continuous_assignment;

// Note: We define the enum with a fixed width integer type for clarity of the
// exposed ABI.
typedef int32_t xls_vast_file_type;
enum {
  xls_vast_file_type_verilog,
  xls_vast_file_type_system_verilog,
};

// Note: caller owns the returned verilog file object, to be freed by
// `xls_vast_verilog_file_free`.
XLS_DLL_EXPORT
struct xls_vast_verilog_file* xls_vast_make_verilog_file(
    xls_vast_file_type file_type);

XLS_DLL_EXPORT
void xls_vast_verilog_file_free(struct xls_vast_verilog_file* f);

XLS_DLL_EXPORT
struct xls_vast_verilog_module* xls_vast_verilog_file_add_module(
    struct xls_vast_verilog_file* f, const char* name);

XLS_DLL_EXPORT
struct xls_vast_data_type* xls_vast_verilog_file_make_scalar_type(
    struct xls_vast_verilog_file* f);

XLS_DLL_EXPORT
struct xls_vast_data_type* xls_vast_verilog_file_make_bit_vector_type(
    struct xls_vast_verilog_file* f, int64_t bit_count, bool is_signed);

XLS_DLL_EXPORT
void xls_vast_verilog_module_add_member_instantiation(
    struct xls_vast_verilog_module* m, struct xls_vast_instantiation* member);
XLS_DLL_EXPORT
void xls_vast_verilog_module_add_member_continuous_assignment(
    struct xls_vast_verilog_module* m,
    struct xls_vast_continuous_assignment* member);

XLS_DLL_EXPORT
struct xls_vast_logic_ref* xls_vast_verilog_module_add_input(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type);
XLS_DLL_EXPORT
struct xls_vast_logic_ref* xls_vast_verilog_module_add_output(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type);
XLS_DLL_EXPORT
struct xls_vast_logic_ref* xls_vast_verilog_module_add_wire(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type);
// TODO(cdleary): 2024-09-05 Add xls_vast_verilog_module_add_wire_with_expr

XLS_DLL_EXPORT
struct xls_vast_continuous_assignment*
xls_vast_verilog_file_make_continuous_assignment(
    struct xls_vast_verilog_file* f, struct xls_vast_expression* lhs,
    struct xls_vast_expression* rhs);

XLS_DLL_EXPORT
struct xls_vast_instantiation* xls_vast_verilog_file_make_instantiation(
    struct xls_vast_verilog_file* f, const char* module_name,
    const char* instance_name, const char** parameter_port_names,
    struct xls_vast_expression** parameter_expressions, size_t parameter_count,
    const char** connection_port_names,
    struct xls_vast_expression** connection_expressions,
    size_t connection_count);

XLS_DLL_EXPORT
void xls_vast_verilog_file_add_include(struct xls_vast_verilog_file* f,
                                       const char* path);

XLS_DLL_EXPORT
struct xls_vast_slice* xls_vast_verilog_file_make_slice_i64(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject, int64_t hi, int64_t lo);

XLS_DLL_EXPORT
struct xls_vast_literal* xls_vast_verilog_file_make_plain_literal(
    struct xls_vast_verilog_file* f, int32_t value);

// Creates a VAST literal with an arbitrary bit count.
//
// Returns an error if the given format preference is invalid.
bool xls_vast_verilog_file_make_literal(struct xls_vast_verilog_file* f,
                                        struct xls_bits* bits,
                                        xls_format_preference format_preference,
                                        bool emit_bit_count, char** error_out,
                                        struct xls_vast_literal** literal_out);

// Casts to turn the given node to an expression, where possible.
XLS_DLL_EXPORT
struct xls_vast_expression* xls_vast_literal_as_expression(
    struct xls_vast_literal* v);
XLS_DLL_EXPORT
struct xls_vast_expression* xls_vast_logic_ref_as_expression(
    struct xls_vast_logic_ref* v);
XLS_DLL_EXPORT
struct xls_vast_expression* xls_vast_slice_as_expression(
    struct xls_vast_slice* v);

XLS_DLL_EXPORT
struct xls_vast_indexable_expression*
xls_vast_logic_ref_as_indexable_expression(
    struct xls_vast_logic_ref* logic_ref);

// Emits/formats the contents of the given verilog file to a string.
//
// Note: caller owns the returned string, to be freed by `xls_c_str_free`.
XLS_DLL_EXPORT
char* xls_vast_verilog_file_emit(const struct xls_vast_verilog_file* f);

}  // extern "C"

#endif  // XLS_PUBLIC_C_API_H_
