/*
Serval DNA configuration
Copyright (C) 2012 Serval Project Inc.
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/* This file defines the internal API to the configuration file.  See "conf_schema.h" for the
 * definition of the configuration schema, which is used to generate these API components.
 *
 * Each STRUCT(NAME, ...) schema declaration generates the following data declaration:
 *
 *      struct config_NAME {
 *          ...
 *      };
 *
 *      A C struct definition containing exactly one element per schema declaration inside the
 *      STRUCT..END_STRUCT block, in the defined order.  The TYPE and NAME of each element depends
 *      on the schema declaration that produces it:
 *
 *      ATOM(TYPE, bar, ...)
 *      NODE(TYPE, bar, ...)
 *
 *          TYPE bar;
 *
 *      STRING(SIZE, bar, ...)
 *
 *          char bar[SIZE+1];
 *
 *      SUB_STRUCT(NAME, bar, ...)
 *      NODE_STRUCT(NAME, bar, ...)
 *      
 *          struct config_NAME bar;
 *
 * Each ARRAY(NAME, ...) ... END_ARRAY(SIZE) schema declaration produces the following data
 * declaration:
 *
 *      struct config_NAME {
 *          unsigned ac;
 *          struct config_NAME__element {
 *              KEY-DECLARATION;
 *              VALUE-DECLARATION;
 *          } av[SIZE];
 *      };
 *
 *      A C struct definition containing a count 'ac' of the number of array elements 0..SIZE-1,
 *      and 'av' an array of element values, each one consisting of a key and a value:
 *
 *      KEY_ATOM(TYPE, ...)
 *
 *              TYPE key;
 *      
 *      KEY_STRING(SIZE, ...)
 *
 *              char key[SIZE+1];
 *      
 *      VALUE_ATOM(NAME, SIZE, LABELLEN, TYPE, ...)
 *      VALUE_NODE(NAME, SIZE, LABELLEN, TYPE, ...)
 *
 *              TYPE value;
 *
 *      VALUE_STRING(STRINGSIZE, ...)
 *
 *              char value[STRINGSIZE+1];
 *
 *      VALUE_SUB_STRUCT(STRUCTNAME)
 *      VALUE_NODE_STRUCT(STRUCTNAME, ...)
 *
 *              struct config_STRUCTNAME value;
 *
 * Each STRUCT(NAME, ...) and ARRAY(NAME, ...) schema declaration generates the following API
 * functions:
 *
 *  - int cf_dfl_config_NAME(struct config_NAME *dest);
 *
 *      A C function which sets the entire contents of the given C structure to its default values
 *      as defined in the schema.  This will only return CFOK or CFERROR; see below.
 *
 *  - int cf_opt_config_NAME(struct config_NAME *dest, const struct cf_om_node *node);
 *
 *      A C function which parses the given COM (configuration object model) and assigns the parsed
 *      result into the given C structure.  See below for the return value.  For arrays, this
 *      function is used to parse each individual array element, and the parsed result is only
 *      appended to the array if it returns CFOK.
 *
 * If a STRUCT(NAME, VALIDATOR) or ARRAY(NAME, FLAGS, VALIDATOR) schema declaration is given a
 * validator function, then the function must have the following signature:
 *
 *  - int VALIDATOR(struct config_NAME *dest, int orig_result);
 *
 *      A C function which validates the contents of the given C structure (struct or array) as
 *      defined in the schema.  This function is invoked by the cf_opt_config_NAME() parser function
 *      just before it returns, so all the parse functions have already been called and the result
 *      is assembled.  The validator function is passed a pointer to the (non-const) structure,
 *      which it may modify if desired, and the original CFxxx flags result code (not CFERROR) that
 *      would be returned by the cf_opt_config_NAME() parser function.  It returns a new CFxxx flags
 *      result (which may simply be the same as was passed).
 *
 *      In the case arrays, validator() is passed a *dest containing elements that were successfully
 *      parsed from the COM, omitting any that did not parse successfully (in which case the
 *      relevant CFxxx result flags will be set) and arbitrarily omitting others that did not fit
 *      (in which case the CFOVERFLOW flag is set).  It is up to validator() to decide whether to
 *      return some, all or none of these elements (ie, alter dest->ac and/or dest->av), and whether
 *      to set or clear the CFARRAYOVERFLOW bit, or set other bits (like CFINVALID for example).  If
 *      there is no validator function, then cf_opt_config_NAME() will return an empty array (dest->ac
 *      == 0) in the case of CFARRAYOVERFLOW.
 *
 * All parse functions assign the result of their parsing into the struct given in their 'dest'
 * argument, and return a bitmask of the following flags:
 *
 *  - CFERROR (all bits set, == -1) if an unrecoverable error occurs (eg, malloc() fails).  The
 *    result in *dest is undefined and may be malformed or inconsistent.
 *
 *  - CFEMPTY if no items were parsed from the COM.  In the case of a struct, this means that no
 *    child nodes were found for any elements; if any child nodes were present but failed parsing
 *    then CFEMPTY is not set but other flags will be set.  In the case of arrays, CFEMPTY means
 *    that the returned array has zero length for _any_ reason (overflow, element parsing failures,
 *    or no elements present in the COM).
 *
 *  - CFUNSUPPORTED if the config item (array or struct) is not supported.  This flag is not
 *    produced by the normal cf_opt_config_NAME() parse functions, but a validation function could set
 *    it to indicate that a given option is not yet implemented or has been deprecated.  In that
 *    case, the validation function should also log a message to that effect.  The CFUNSUPPORTED
 *    flag is mainly used in its CFSUB(CFUNSUPPORTED) form (see below) to indicate that the COM
 *    contains elements that are not defined in the STRUCT.  This may indicate a typo in the name
 *    of a config option, resulting in the intended option not being set.
 *
 *  - CFDUPLICATE if a duplicate array entry was found.  The result may be an empty array (in which
 *    case CFEMPTY is also set), or an array that omits the duplicate element.  It is not defined
 *    which of the two conflicting elements will get omitted.  Normal array parsing without a
 *    validator function will return an empty array in the case of duplicate, but a validator
 *    function may change this behaviour.
 *
 *  - CFARRAYOVERFLOW if the size of any array was exceeded.  The result in *dest may be empty (in
 *    which case CFEMPTY is also set), or may contain elements parsed successfully from the COM (ie,
 *    returned CFOK), omitting any that did not parse successfully (in which case the relevant
 *    CFSUB() bits will be set) and arbitrarily omitting others that did not fit.  It is not defined
 *    which elements get omitted from an overflowed array.  Normal array parsing without a validator
 *    function will return an empty array in the case of overflow, but a validator function may
 *    change this behaviour.
 *
 *  - CFSTRINGFOVERFLOW if the size of any string element was exceeded.  The result in *dest may be
 *    unchanged or may contain a truncated string, depending on the parser that detected and
 *    reported the string overflow.
 *
 *  - CFINCOMPLETE if any MANDATORY element is missing (no node in the COM) or empty (as indicated
 *    by the CFEMPTY bit in its parse result).  The result in *dest is valid but the missing
 *    mandatory element(s) are unchanged (in the case of a struct) or zero-length (in the case of an
 *    array).
 *
 *  - CFINVALID if any invalid configuration value was encountered, ie, any parse function returned
 *    CFINVALID in its return flags.  The result in *dest is valid and the elements that failed
 *    to parse are unchanged.
 *
 *  - CFSUB(CFxxx) if any element of a STRUCT or ARRAY produced a CFxxx result when being parsed, ie
 *    any element's parse function returned CFxxx.  In the case of a STRUCT, the failed elements are
 *    usually left with their prior (default) values, but this depends on the parse functions'
 *    behaviours.  In the case of an ARRAY, failed elements are omitted from the array
 *
 * The difference between CFSUB(CFxxx) and CFxxx needs explaining.  To illustrate, CFSUB(CFINVALID)
 * is different from CFINVALID because an element of a struct or array may have failed to parse, yet
 * the whole struct or array itself may still be valid (in the case of a struct, the element's prior
 * value may be retained, and in the case of an array, the failed element is simply omitted from the
 * result).  A validator function may wish to reflect any CFSUB() bit as a CFINVALID result, but the
 * normal behaviour of cf_opt_config_NAME() is to not return CFINVALID unless the validator function
 * sets it.
 *
 * The special value CFOK is zero (no bits set); in this case a valid result is produced and all of
 * *dest is overwritten (except unused array elements).
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */

#ifndef __SERVALDNA_CONFIG_H
#define __SERVALDNA_CONFIG_H

#include <stdint.h>
#include <arpa/inet.h>

#include "constants.h"
#include "strbuf.h"
#include "serval.h"
#include "rhizome.h"

#define CONFIG_FILE_MAX_SIZE        (32 * 1024)
#define INTERFACE_NAME_STRLEN       40

/* Return bit flags for config schema default cf_dfl_xxx() and parsing cf_opt_xxx() functions. */

#define CFERROR             (~0) // all set
#define CFOK                0
#define CFEMPTY             (1<<0)
#define CFDUPLICATE         (1<<1)
#define CFARRAYOVERFLOW     (1<<2)
#define CFSTRINGOVERFLOW    (1<<3)
#define CFINCOMPLETE        (1<<4)
#define CFINVALID           (1<<5)
#define CFUNSUPPORTED       (1<<6)
#define CF__SUB_SHIFT       16
#define CFSUB(f)            ((f) << CF__SUB_SHIFT)
#define CF__SUBFLAGS        CFSUB(~0)
#define CF__FLAGS           (~0 & ~CF__SUBFLAGS)

strbuf strbuf_cf_flags(strbuf, int);
strbuf strbuf_cf_flag_reason(strbuf sb, int flags);

/* The Configuration Object Model (COM).  The config file is parsed into a tree of these structures
 * first, then those structures are passed as arguments to the schema parsing functions.
 */

struct cf_om_node {
    const char *source; // = parse_config() 'source' arg
    unsigned int line_number;
    const char *fullkey; // malloc()
    const char *key; // points inside fullkey, do not free()
    const char *text; // malloc()
    size_t nodc;
    struct cf_om_node *nodv[10]; // malloc()
};

int is_configvarname(const char *);
int cf_om_parse(const char *source, const char *buf, size_t len, struct cf_om_node **rootp);
int cf_om_get_child(const struct cf_om_node *parent, const char *key, const char *keyend);
const char *cf_om_get(const struct cf_om_node *root, const char *fullkey);
int cf_om_set(struct cf_om_node **nodep, const char *fullkey, const char *text);
void cf_om_free_node(struct cf_om_node **nodep);
void cf_om_dump_node(const struct cf_om_node *node, int indent);

struct cf_om_iterator {
    const struct cf_om_node *node;
    unsigned sp;
    struct {
        const struct cf_om_node *node;
        unsigned index;
    } stack[20];
};

void cf_om_iter_start(struct cf_om_iterator *, const struct cf_om_node *);
int cf_om_iter_next(struct cf_om_iterator *);

struct cf_om_node *cf_om_load();
struct cf_om_node *cf_om_reload();
int cf_om_save(const struct cf_om_node *root);

/* Diagnostic functions for use in config schema parsing functions, cf_opt_xxx(). */

void _cf_warn_nodev(struct __sourceloc __whence, const struct cf_om_node *node, const char *key, const char *fmt, va_list ap);
void _cf_warn_childrenv(struct __sourceloc __whence, const struct cf_om_node *parent, const char *fmt, va_list ap);
void _cf_warn_node(struct __sourceloc __whence, const struct cf_om_node *node, const char *key, const char *fmt, ...);
void _cf_warn_children(struct __sourceloc __whence, const struct cf_om_node *node, const char *fmt, ...);
void _cf_warn_duplicate_node(struct __sourceloc __whence, const struct cf_om_node *parent, const char *key);
void _cf_warn_missing_node(struct __sourceloc __whence, const struct cf_om_node *parent, const char *key);
void _cf_warn_node_value(struct __sourceloc __whence, const struct cf_om_node *node, int reason);
void _cf_warn_no_array(struct __sourceloc __whence, const struct cf_om_node *node, int reason);
void _cf_warn_unsupported_node(struct __sourceloc __whence, const struct cf_om_node *node);
void _cf_warn_unsupported_children(struct __sourceloc __whence, const struct cf_om_node *parent);
void _cf_warn_list_overflow(struct __sourceloc __whence, const struct cf_om_node *node);
void _cf_warn_spurious_children(struct __sourceloc __whence, const struct cf_om_node *parent);
void _cf_warn_array_key(struct __sourceloc __whence, const struct cf_om_node *node, int reason);
void _cf_warn_array_value(struct __sourceloc __whence, const struct cf_om_node *node, int reason);

#define cf_warn_nodev(node, key, fmt, ap)    _cf_warn_nodev(__WHENCE__, node, key, fmt, ap)
#define cf_warn_childrenv(parent, fmt, ap)   _cf_warn_childrenv(__WHENCE__, parent, fmt, ap)
#define cf_warn_node(node, key, fmt, ...)    _cf_warn_node(__WHENCE__, node, key, fmt, ##__VA_ARGS__)
#define cf_warn_children(node, fmt, ...)     _cf_warn_children(__WHENCE__, node, fmt, ##__VA_ARGS__)
#define cf_warn_duplicate_node(parent, key)  _cf_warn_duplicate_node(__WHENCE__, parent, key)
#define cf_warn_missing_node(parent, key)    _cf_warn_missing_node(__WHENCE__, parent, key)
#define cf_warn_node_value(node, reason)     _cf_warn_node_value(__WHENCE__, node, reason)
#define cf_warn_no_array(node, reason)	     _cf_warn_no_array(__WHENCE__, node, reason)
#define cf_warn_unsupported_node(node)	     _cf_warn_unsupported_node(__WHENCE__, node)
#define cf_warn_unsupported_children(parent) _cf_warn_unsupported_children(__WHENCE__, parent)
#define cf_warn_list_overflow(node)	     _cf_warn_list_overflow(__WHENCE__, node)
#define cf_warn_spurious_children(parent)    _cf_warn_spurious_children(__WHENCE__, parent)
#define cf_warn_array_key(node, reason)	     _cf_warn_array_key(__WHENCE__, node, reason)
#define cf_warn_array_value(node, reason)    _cf_warn_array_value(__WHENCE__, node, reason)

struct pattern_list {
    unsigned patc;
    char patv[16][INTERFACE_NAME_STRLEN + 1];
};

#define PATTERN_LIST_EMPTY ((struct pattern_list){.patc = 0})

// Generate config struct definitions, struct config_NAME.
#define STRUCT(__name, __validator...) \
    struct config_##__name {
#define NODE(__type, __element, __default, __parser, __flags, __comment) \
        __type __element;
#define ATOM(__type, __element, __default, __parser, __flags, __comment) \
        __type __element;
#define STRING(__size, __element, __default, __parser, __flags, __comment) \
        char __element[__size + 1];
#define SUB_STRUCT(__name, __element, __flags) \
        struct config_##__name __element;
#define NODE_STRUCT(__name, __element, __parser, __flags) \
        struct config_##__name __element;
#define END_STRUCT \
    };
#define ARRAY(__name, __flags, __validator...) \
    struct config_##__name { \
        unsigned ac; \
        struct config_##__name##__element {
#define KEY_ATOM(__type, __eltparser, __cmpfunc...) \
            __type key;
#define KEY_STRING(__strsize, __eltparser, __cmpfunc...) \
            char key[(__strsize) + 1];
#define VALUE_ATOM(__type, __eltparser) \
            __type value;
#define VALUE_STRING(__strsize, __eltparser) \
            char value[(__strsize) + 1];
#define VALUE_NODE(__type, __eltparser) \
            __type value;
#define VALUE_SUB_STRUCT(__structname) \
            struct config_##__structname value;
#define VALUE_NODE_STRUCT(__structname, __eltparser) \
            struct config_##__structname value;
#define END_ARRAY(__size) \
        } av[(__size)]; \
    };
#include "conf_schema.h"
#undef STRUCT
#undef NODE
#undef ATOM
#undef STRING
#undef SUB_STRUCT
#undef NODE_STRUCT
#undef END_STRUCT
#undef ARRAY
#undef KEY_ATOM
#undef KEY_STRING
#undef VALUE_ATOM
#undef VALUE_STRING
#undef VALUE_NODE
#undef VALUE_SUB_STRUCT
#undef VALUE_NODE_STRUCT
#undef END_ARRAY

// Generate config set-default function prototypes, cf_dfl_config_NAME().
#define STRUCT(__name, __validator...) \
    int cf_dfl_config_##__name(struct config_##__name *s);
#define NODE(__type, __element, __default, __parser, __flags, __comment)
#define ATOM(__type, __element, __default, __parser, __flags, __comment)
#define STRING(__size, __element, __default, __parser, __flags, __comment)
#define SUB_STRUCT(__name, __element, __flags)
#define NODE_STRUCT(__name, __element, __parser, __flags)
#define END_STRUCT
#define ARRAY(__name, __flags, __validator...) \
    int cf_dfl_config_##__name(struct config_##__name *a);
#define KEY_ATOM(__type, __eltparser, __cmpfunc...)
#define KEY_STRING(__strsize, __eltparser, __cmpfunc...)
#define VALUE_ATOM(__type, __eltparser)
#define VALUE_STRING(__strsize, __eltparser)
#define VALUE_NODE(__type, __eltparser)
#define VALUE_SUB_STRUCT(__structname)
#define VALUE_NODE_STRUCT(__structname, __eltparser)
#define END_ARRAY(__size)
#include "conf_schema.h"
#undef STRUCT
#undef NODE
#undef ATOM
#undef STRING
#undef SUB_STRUCT
#undef NODE_STRUCT
#undef END_STRUCT
#undef ARRAY
#undef KEY_ATOM
#undef KEY_STRING
#undef VALUE_ATOM
#undef VALUE_STRING
#undef VALUE_NODE
#undef VALUE_SUB_STRUCT
#undef VALUE_NODE_STRUCT
#undef END_ARRAY

// Generate config parser function prototypes.
#define __VALIDATOR(__name, __validator...) \
    typedef int __validator_func__config_##__name##__t(const struct cf_om_node *, struct config_##__name *, int); \
    __validator_func__config_##__name##__t __dummy__validator_func__config_##__name, ##__validator;
#define STRUCT(__name, __validator...) \
    int cf_opt_config_##__name(struct config_##__name *, const struct cf_om_node *); \
    __VALIDATOR(__name, ##__validator)
#define NODE(__type, __element, __default, __parser, __flags, __comment) \
    int __parser(__type *, const struct cf_om_node *);
#define ATOM(__type, __element, __default, __parser, __flags, __comment) \
    int __parser(__type *, const char *);
#define STRING(__size, __element, __default, __parser, __flags, __comment) \
    int __parser(char *, size_t, const char *);
#define SUB_STRUCT(__name, __element, __flags) \
    int cf_opt_config_##__name(struct config_##__name *, const struct cf_om_node *);
#define NODE_STRUCT(__name, __element, __parser, __flags) \
    int __parser(struct config_##__name *, const struct cf_om_node *);
#define END_STRUCT
#define ARRAY(__name, __flags, __validator...) \
    int cf_opt_config_##__name(struct config_##__name *, const struct cf_om_node *); \
    __VALIDATOR(__name, ##__validator)
#define KEY_ATOM(__type, __eltparser, __cmpfunc...) \
    int __eltparser(__type *, const char *);
#define KEY_STRING(__strsize, __eltparser, __cmpfunc...) \
    int __eltparser(char *, size_t, const char *);
#define VALUE_ATOM(__type, __eltparser) \
    int __eltparser(__type *, const char *);
#define VALUE_STRING(__strsize, __eltparser) \
    int __eltparser(char *, size_t, const char *);
#define VALUE_NODE(__type, __eltparser) \
    int __eltparser(__type *, const struct cf_om_node *);
#define VALUE_SUB_STRUCT(__structname) \
    int cf_opt_config_##__structname(struct config_##__structname *, const struct cf_om_node *);
#define VALUE_NODE_STRUCT(__structname, __eltparser) \
    int __eltparser(struct config_##__structname *, const struct cf_om_node *);
#define END_ARRAY(__size)
#include "conf_schema.h"
#undef __VALIDATOR
#undef STRUCT
#undef NODE
#undef ATOM
#undef STRING
#undef SUB_STRUCT
#undef NODE_STRUCT
#undef END_STRUCT
#undef ARRAY
#undef KEY_ATOM
#undef KEY_STRING
#undef VALUE_ATOM
#undef VALUE_STRING
#undef VALUE_NODE
#undef VALUE_SUB_STRUCT
#undef VALUE_NODE_STRUCT
#undef END_ARRAY

// Generate config array key comparison function prototypes.
#define STRUCT(__name, __validator...)
#define NODE(__type, __element, __default, __parser, __flags, __comment)
#define ATOM(__type, __element, __default, __parser, __flags, __comment)
#define STRING(__size, __element, __default, __parser, __flags, __comment)
#define SUB_STRUCT(__name, __element, __flags)
#define NODE_STRUCT(__name, __element, __parser, __flags)
#define END_STRUCT
#define ARRAY(__name, __flags, __validator...) \
    typedef int __compare_func__config_##__name##__t
#define KEY_ATOM(__type, __eltparser, __cmpfunc...) \
        (const __type *, const __type *);
#define KEY_STRING(__strsize, __eltparser, __cmpfunc...) \
        (const char *, const char *);
#define VALUE_ATOM(__type, __eltparser)
#define VALUE_STRING(__strsize, __eltparser)
#define VALUE_NODE(__type, __eltparser)
#define VALUE_SUB_STRUCT(__structname)
#define VALUE_NODE_STRUCT(__structname, __eltparser)
#define END_ARRAY(__size)
#include "conf_schema.h"
#undef ARRAY
#undef KEY_ATOM
#undef KEY_STRING
#define ARRAY(__name, __flags, __validator...) \
    __compare_func__config_##__name##__t __dummy__compare_func__config_##__name
#define KEY_ATOM(__type, __eltparser, __cmpfunc...) \
        ,##__cmpfunc;
#define KEY_STRING(__strsize, __eltparser, __cmpfunc...) \
        ,##__cmpfunc;
#include "conf_schema.h"
#undef STRUCT
#undef NODE
#undef ATOM
#undef STRING
#undef SUB_STRUCT
#undef NODE_STRUCT
#undef END_STRUCT
#undef ARRAY
#undef KEY_ATOM
#undef KEY_STRING
#undef VALUE_ATOM
#undef VALUE_STRING
#undef VALUE_NODE
#undef VALUE_SUB_STRUCT
#undef VALUE_NODE_STRUCT
#undef END_ARRAY

// Generate config array search-by-key function prototypes.
#define STRUCT(__name, __validator...)
#define NODE(__type, __element, __default, __parser, __flags, __comment)
#define ATOM(__type, __element, __default, __parser, __flags, __comment)
#define STRING(__size, __element, __default, __parser, __flags, __comment)
#define SUB_STRUCT(__name, __element, __flags)
#define NODE_STRUCT(__name, __element, __parser, __flags)
#define END_STRUCT
#define ARRAY(__name, __flags, __validator...) \
    int config_##__name##__get(const struct config_##__name *,
#define KEY_ATOM(__type, __eltparser, __cmpfunc...) \
        const __type *);
#define KEY_STRING(__strsize, __eltparser, __cmpfunc...) \
        const char *);
#define VALUE_ATOM(__type, __eltparser)
#define VALUE_STRING(__strsize, __eltparser)
#define VALUE_NODE(__type, __eltparser)
#define VALUE_SUB_STRUCT(__structname)
#define VALUE_NODE_STRUCT(__structname, __eltparser)
#define END_ARRAY(__size)
#include "conf_schema.h"
#undef STRUCT
#undef NODE
#undef ATOM
#undef STRING
#undef SUB_STRUCT
#undef NODE_STRUCT
#undef END_STRUCT
#undef ARRAY
#undef KEY_ATOM
#undef KEY_STRING
#undef VALUE_ATOM
#undef VALUE_STRING
#undef VALUE_NODE
#undef VALUE_SUB_STRUCT
#undef VALUE_NODE_STRUCT
#undef END_ARRAY

int cf_opt_boolean(int *booleanp, const char *text);
int cf_opt_absolute_path(char *str, size_t len, const char *text);
int cf_opt_debugflags(debugflags_t *flagsp, const struct cf_om_node *node);
int cf_opt_rhizome_peer(struct config_rhizome_peer *, const struct cf_om_node *node);
int cf_opt_rhizome_peer_from_uri(struct config_rhizome_peer *, const char *uri);
int cf_opt_str(char *str, size_t len, const char *text);
int cf_opt_str_nonempty(char *str, size_t len, const char *text);
int cf_opt_int(int *intp, const char *text);
int cf_opt_int32_nonneg(int32_t *intp, const char *text);
int cf_opt_uint32_nonzero(uint32_t *intp, const char *text);
int cf_opt_uint64_scaled(uint64_t *intp, const char *text);
int cf_opt_protocol(char *str, size_t len, const char *text);
int cf_opt_in_addr(struct in_addr *addrp, const char *text);
int cf_opt_port(unsigned short *portp, const char *text);
int cf_opt_sid(sid_t *sidp, const char *text);
int cf_opt_rhizome_bk(rhizome_bk_t *bkp, const char *text);
int cf_opt_interface_type(short *typep, const char *text);
int cf_opt_pattern_list(struct pattern_list *listp, const char *text);
int cf_opt_interface_list(struct config_interface_list *listp, const struct cf_om_node *node);

extern int cf_limbo;
extern struct config_main config;

int cf_init();
int cf_load();
int cf_reload();

#endif //__SERVALDNA_CONFIG_H
