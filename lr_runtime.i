#line 1 "F:\\GitHub\\LR_JS\\src\\lr_runtime.c"
/*
 * L/R_JS - Runtime Implementation
 * Pure C, ES2022-compatible, high-performance JS runtime.
 */
#line 1 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
/*
 * L/R_JS - Cross-Platform Compatibility Layer
 * Supports: Linux, macOS, Windows 7+, BSD
 *
 * This header provides a uniform API across all platforms,
 * mapping POSIX calls to their Windows equivalents.
 *
 * All headers MUST include lr_platform.h instead of <pthread.h>
 * directly to ensure MSVC compatibility.
 */



/* 鈹€鈹€ Standard headers (common to all platforms) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
//
// stdlib.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <stdlib.h> header.
//
#pragma once



#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
//
// corecrt.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Declarations used throughout the CoreCRT library.
//
#pragma once

#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
//
// vcruntime.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Declarations used throughout the VCRuntime library.
//
#pragma once
//
// Note on use of "deprecate":
//
// Various places in this header and other headers use
// __declspec(deprecate) or macros that have the term DEPRECATE in them.
// We use "deprecate" here ONLY to signal the compiler to emit a warning
// about these items. The use of "deprecate" should NOT be taken to imply
// that any standard committee has deprecated these functions from the
// relevant standards.  In fact, these functions are NOT deprecated from
// the standard.
//
// Full details can be found in our documentation by searching for
// "Security Enhancements in the CRT".
//




// Many VCRuntime headers avoid exposing their contents to non-compilers like
// the Windows resource compiler and Qt's meta-object compiler (moc).


#line 32 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

#line 34 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 35 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    
#line 39 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// The _CRTIMP macro is not used in the VCRuntime or the CoreCRT anymore, but
// there is a lot of existing code that declares CRT functions using this macro,
// and if we remove its definition, we break that existing code.  It is thus
// defined here only for compatibility.

    
    

#line 49 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        


            
        #line 54 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
    #line 55 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 56 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
/***
*sal.h - markers for documenting the semantics of APIs
*
*       Copyright (c) Microsoft Corporation. All rights reserved.
*
*Purpose:
*       sal.h provides a set of annotations to describe how a function uses its
*       parameters - the assumptions it makes about them, and the guarantees it makes
*       upon finishing.
*
*       [Public]
*
****/
#pragma once

/*==========================================================================

   The comments in this file are intended to give basic understanding of
   the usage of SAL, the Microsoft Source Code Annotation Language.
   For more details, please see https://go.microsoft.com/fwlink/?LinkID=242134

   The macros are defined in 3 layers, plus the structural set:

   _In_/_Out_/_Ret_ Layer:
   ----------------------
   This layer provides the highest abstraction and its macros should be used
   in most cases. These macros typically start with:
      _In_     : input parameter to a function, unmodified by called function
      _Out_    : output parameter, written to by called function, pointed-to
                 location not expected to be initialized prior to call
      _Outptr_ : like _Out_ when returned variable is a pointer type
                 (so param is pointer-to-pointer type). Called function
                 provides/allocated space.
      _Outref_ : like _Outptr_, except param is reference-to-pointer type.
      _Inout_  : inout parameter, read from and potentially modified by
                 called function.
      _Ret_    : for return values
      _Field_  : class/struct field invariants
   For common usage, this class of SAL provides the most concise annotations.
   Note that _In_/_Out_/_Inout_/_Outptr_ annotations are designed to be used
   with a parameter target. Using them with _At_ to specify non-parameter
   targets may yield unexpected results.

   This layer also includes a number of other properties that can be specified
   to extend the ability of code analysis, most notably:
      -- Designating parameters as format strings for printf/scanf/scanf_s
      -- Requesting stricter type checking for C enum parameters

   _Pre_/_Post_ Layer:
   ------------------
   The macros of this layer only should be used when there is no suitable macro
   in the _In_/_Out_ layer. Its macros start with _Pre_ or _Post_.
   This layer provides the most flexibility for annotations.

   Implementation Abstraction Layer:
   --------------------------------
   Macros from this layer should never be used directly. The layer only exists
   to hide the implementation of the annotation macros.

   Structural Layer:
   ----------------
   These annotations, like _At_ and _When_, are used with annotations from
   any of the other layers as modifiers, indicating exactly when and where
   the annotations apply.


   Common syntactic conventions:
   ----------------------------

   Usage:
   -----
   _In_, _Out_, _Inout_, _Pre_, _Post_, are for formal parameters.
   _Ret_, _Deref_ret_ must be used for return values.

   Nullness:
   --------
   If the parameter can be NULL as a precondition to the function, the
   annotation contains _opt. If the macro does not contain '_opt' the
   parameter cannot be NULL.

   If an out/inout parameter returns a null pointer as a postcondition, this is
   indicated by _Ret_maybenull_ or _result_maybenull_. If the macro is not
   of this form, then the result will not be NULL as a postcondition.
     _Outptr_ - output value is not NULL
     _Outptr_result_maybenull_ - output value might be NULL

   String Type:
   -----------
   _z: NullTerminated string
   for _In_ parameters the buffer must have the specified stringtype before the call
   for _Out_ parameters the buffer must have the specified stringtype after the call
   for _Inout_ parameters both conditions apply

   Extent Syntax:
   -------------
   Buffer sizes are expressed as element counts, unless the macro explicitly
   contains _byte_ or _bytes_. Some annotations specify two buffer sizes, in
   which case the second is used to indicate how much of the buffer is valid
   as a postcondition. This table outlines the precondition buffer allocation
   size, precondition number of valid elements, postcondition allocation size,
   and postcondition number of valid elements for representative buffer size
   annotations:
                                     Pre    |  Pre    |  Post   |  Post
                                     alloc  |  valid  |  alloc  |  valid
      Annotation                     elems  |  elems  |  elems  |  elems
      ----------                     ------------------------------------
      _In_reads_(s)                    s    |   s     |   s     |   s
      _Inout_updates_(s)               s    |   s     |   s     |   s
      _Inout_updates_to_(s,c)          s    |   s     |   s     |   c
      _Out_writes_(s)                  s    |   0     |   s     |   s
      _Out_writes_to_(s,c)             s    |   0     |   s     |   c
      _Outptr_result_buffer_(s)        ?    |   ?     |   s     |   s
      _Outptr_result_buffer_to_(s,c)   ?    |   ?     |   s     |   c

   For the _Outptr_ annotations, the buffer in question is at one level of
   dereference. The called function is responsible for supplying the buffer.

   Success and failure:
   -------------------
   The SAL concept of success allows functions to define expressions that can
   be tested by the caller, which if it evaluates to non-zero, indicates the
   function succeeded, which means that its postconditions are guaranteed to
   hold.  Otherwise, if the expression evaluates to zero, the function is
   considered to have failed, and the postconditions are not guaranteed.

   The success criteria can be specified with the _Success_(expr) annotation:
     _Success_(return != FALSE) BOOL
     PathCanonicalizeA(_Out_writes_(MAX_PATH) LPSTR pszBuf, LPCSTR pszPath) :
        pszBuf is only guaranteed to be NULL-terminated when TRUE is returned,
        and FALSE indicates failure. In common practice, callers check for zero
        vs. non-zero returns, so it is preferable to express the success
        criteria in terms of zero/non-zero, not checked for exactly TRUE.

   Functions can specify that some postconditions will still hold, even when
   the function fails, using _On_failure_(anno-list), or postconditions that
   hold regardless of success or failure using _Always_(anno-list).

   The annotation _Return_type_success_(expr) may be used with a typedef to
   give a default _Success_ criteria to all functions returning that type.
   This is the case for common Windows API status types, including
   HRESULT and NTSTATUS.  This may be overridden on a per-function basis by
   specifying a _Success_ annotation locally.

============================================================================*/





#line 151 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"



#line 155 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

























// Disable expansion of SAL macros in non-Prefast mode to
// improve compiler throughput.


#line 185 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"


#line 188 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

#line 190 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

// safeguard for MIDL and RC builds



#line 196 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"



#line 200 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






#line 207 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"











#line 219 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






// Some annotations aren't officially SAL2 yet.

#line 228 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
#line 229 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"


//============================================================================
//   Structural SAL:
//     These annotations modify the use of other annotations.  They may
//     express the annotation target (i.e. what parameter/field the annotation
//     applies to) or the condition under which the annotation is applicable.
//============================================================================

// _At_(target, annos) specifies that the annotations listed in 'annos' is to
// be applied to 'target' rather than to the identifier which is the current
// lexical target.


// _At_buffer_(target, iter, bound, annos) is similar to _At_, except that
// target names a buffer, and each annotation in annos is applied to each
// element of target up to bound, with the variable named in iter usable
// by the annotations to refer to relevant offsets within target.


// _When_(expr, annos) specifies that the annotations listed in 'annos' only
// apply when 'expr' evaluates to non-zero.




// <expr> indicates whether normal post conditions apply to a function


// <expr> indicates whether post conditions apply to a function returning
// the type that this annotation is applied to


// Establish postconditions that apply only if the function does not succeed


// Establish postconditions that apply in both success and failure cases.
// Only applicable with functions that have  _Success_ or _Return_type_succss_.


// Usable on a function defintion. Asserts that a function declaration is
// in scope, and its annotations are to be used. There are no other annotations
// allowed on the function definition.


// _Notref_ may precede a _Deref_ or "real" annotation, and removes one
// level of dereference if the parameter is a C++ reference (&).  If the
// net deref on a "real" annotation is negative, it is simply discarded.


// Annotations for defensive programming styles.







//============================================================================
//   _In_/_Out_ Layer:
//============================================================================

// Reserved pointer parameters, must always be NULL.


// _Const_ allows specification that any namable memory location is considered
// readonly for a given call.



// Input parameters --------------------------

//   _In_ - Annotations for parameters where data is passed into the function, but not modified.
//          _In_ by itself can be used with non-pointer types (although it is redundant).

// e.g. void SetPoint( _In_ const POINT* pPT );



// nullterminated 'in' parameters.
// e.g. void CopyStr( _In_z_ const char* szFrom, _Out_z_cap_(cchTo) char* szTo, size_t cchTo );




// 'input' buffers with given size











// 'input' buffers valid to the given end pointer








// Output parameters --------------------------

//   _Out_ - Annotations for pointer or reference parameters where data passed back to the caller.
//           These are mostly used where the pointer/reference is to a non-pointer type.
//           _Outptr_/_Outref) (see below) are typically used to return pointers via parameters.

// e.g. void GetPoint( _Out_ POINT* pPT );


























// Inout parameters ----------------------------

//   _Inout_ - Annotations for pointer or reference parameters where data is passed in and
//        potentially modified.
//          void ModifyPoint( _Inout_ POINT* pPT );
//          void ModifyPointByRef( _Inout_ POINT& pPT );




// For modifying string buffers
//   void toupper( _Inout_z_ char* sz );



// For modifying buffers with explicit element size











// For modifying buffers with explicit byte size










// Pointer to pointer parameters -------------------------

//   _Outptr_ - Annotations for output params returning pointers
//      These describe parameters where the called function provides the buffer:
//        HRESULT SHStrDupW(_In_ LPCWSTR psz, _Outptr_ LPWSTR *ppwsz);
//      The caller passes the address of an LPWSTR variable as ppwsz, and SHStrDupW allocates
//      and initializes memory and returns the pointer to the new LPWSTR in *ppwsz.
//
//    _Outptr_opt_ - describes parameters that are allowed to be NULL.
//    _Outptr_*_result_maybenull_ - describes parameters where the called function might return NULL to the caller.
//
//    Example:
//       void MyFunc(_Outptr_opt_ int **ppData1, _Outptr_result_maybenull_ int **ppData2);
//    Callers:
//       MyFunc(NULL, NULL);           // error: parameter 2, ppData2, should not be NULL
//       MyFunc(&pData1, &pData2);     // ok: both non-NULL
//       if (*pData1 == *pData2) ...   // error: pData2 might be NULL after call






// Annotations for _Outptr_ parameters returning pointers to null terminated strings.






// Annotations for _Outptr_ parameters where the output pointer is set to NULL if the function fails.




// Annotations for _Outptr_ parameters which return a pointer to a ref-counted COM object,
// following the COM convention of setting the output to NULL on failure.
// The current implementation is identical to _Outptr_result_nullonfailure_.
// For pointers to types that are not COM objects, _Outptr_result_nullonfailure_ is preferred.






// Annotations for _Outptr_ parameters returning a pointer to buffer with a specified number of elements/bytes

































// Annotations for output reference to pointer parameters.


















// Annotations for output reference to pointer parameters that guarantee
// that the pointer is set to NULL on failure.


// Generic annotations to set output value of a by-pointer or by-reference parameter to null/zero on failure.




// return values -------------------------------

//
// _Ret_ annotations
//
// describing conditions that hold for return values after the call

// e.g. _Ret_z_ CString::operator const wchar_t*() const noexcept;



// used with allocated but not yet initialized objects




// used with allocated and initialized objects
//    returns single valid object


//    returns pointer to initialized buffer of specified size







//    returns pointer to partially initialized buffer, with total size 'size' and initialized size 'count'






// Annotations for strict type checking




// Check the return value of a function e.g. _Check_return_ ErrorCode Foo();



// e.g. MyPrintF( _Printf_format_string_ const wchar_t* wzFormat, ... );









// annotations to express value of integral or pointer parameter









// annotation to express that a value (usually a field of a mutable class)
// is not changed by a function call


// Annotations to allow expressing generalized pre and post conditions.
// 'cond' may be any valid SAL expression that is considered to be true as a precondition
// or postcondition (respectively).



// Annotations to express struct, class and field invariants




















//============================================================================
//   _Pre_/_Post_ Layer:
//============================================================================

//
// Raw Pre/Post for declaring custom pre/post conditions
//




//
// Validity property
//





//
// Buffer size properties
//

// Expressing buffer sizes without specifying pre or post condition








// Expressing buffer size as pre or post condition










//
// Pointer null-ness properties
//




//
// _Pre_ annotations ---
//
// describing conditions that must be met before the call of the function

// e.g. int strlen( _Pre_z_ const char* sz );
// buffer is a zero terminated string


// valid size unknown or indicated by type (e.g.:LPSTR)





// Overrides recursive valid when some field is not yet initialized when using _Inout_


// used with allocated but not yet initialized objects




//
// _Post_ annotations ---
//
// describing conditions that hold after the function call

// void CopyStr( _In_z_ const char* szFrom, _Pre_cap_(cch) _Post_z_ char* szFrom, size_t cchFrom );
// buffer will be a zero-terminated string after the call


// e.g. HRESULT InitStruct( _Post_valid_ Struct* pobj );



// e.g. void free( _Post_ptr_invalid_ void* pv );


// e.g. void ThrowExceptionIfNull( _Post_notnull_ const void* pv );


// e.g. HRESULT GetObject(_Outptr_ _On_failure_(_At_(*p, _Post_null_)) T **p);







#pragma region Input Buffer SAL 1 compatibility macros

/*==========================================================================

   This section contains definitions for macros defined for VS2010 and earlier.
   Usage of these macros is still supported, but the SAL 2 macros defined above
   are recommended instead.  This comment block is retained to assist in
   understanding SAL that still uses the older syntax.

   The macros are defined in 3 layers:

   _In_/_Out_ Layer:
   ----------------
   This layer provides the highest abstraction and its macros should be used
   in most cases. Its macros start with _In_, _Out_ or _Inout_. For the
   typical case they provide the most concise annotations.

   _Pre_/_Post_ Layer:
   ------------------
   The macros of this layer only should be used when there is no suitable macro
   in the _In_/_Out_ layer. Its macros start with _Pre_, _Post_, _Ret_,
   _Deref_pre_ _Deref_post_ and _Deref_ret_. This layer provides the most
   flexibility for annotations.

   Implementation Abstraction Layer:
   --------------------------------
   Macros from this layer should never be used directly. The layer only exists
   to hide the implementation of the annotation macros.


   Annotation Syntax:
   |--------------|----------|----------------|-----------------------------|
   |   Usage      | Nullness | ZeroTerminated |  Extent                     |
   |--------------|----------|----------------|-----------------------------|
   | _In_         | <>       | <>             | <>                          |
   | _Out_        | opt_     | z_             | [byte]cap_[c_|x_]( size )   |
   | _Inout_      |          |                | [byte]count_[c_|x_]( size ) |
   | _Deref_out_  |          |                | ptrdiff_cap_( ptr )         |
   |--------------|          |                | ptrdiff_count_( ptr )       |
   | _Ret_        |          |                |                             |
   | _Deref_ret_  |          |                |                             |
   |--------------|          |                |                             |
   | _Pre_        |          |                |                             |
   | _Post_       |          |                |                             |
   | _Deref_pre_  |          |                |                             |
   | _Deref_post_ |          |                |                             |
   |--------------|----------|----------------|-----------------------------|

   Usage:
   -----
   _In_, _Out_, _Inout_, _Pre_, _Post_, _Deref_pre_, _Deref_post_ are for
   formal parameters.
   _Ret_, _Deref_ret_ must be used for return values.

   Nullness:
   --------
   If the pointer can be NULL the annotation contains _opt. If the macro
   does not contain '_opt' the pointer may not be NULL.

   String Type:
   -----------
   _z: NullTerminated string
   for _In_ parameters the buffer must have the specified stringtype before the call
   for _Out_ parameters the buffer must have the specified stringtype after the call
   for _Inout_ parameters both conditions apply

   Extent Syntax:
   |------|---------------|---------------|
   | Unit | Writ/Readable | Argument Type |
   |------|---------------|---------------|
   |  <>  | cap_          | <>            |
   | byte | count_        | c_            |
   |      |               | x_            |
   |------|---------------|---------------|

   'cap' (capacity) describes the writable size of the buffer and is typically used
   with _Out_. The default unit is elements. Use 'bytecap' if the size is given in bytes
   'count' describes the readable size of the buffer and is typically used with _In_.
   The default unit is elements. Use 'bytecount' if the size is given in bytes.

   Argument syntax for cap_, bytecap_, count_, bytecount_:
   (<parameter>|return)[+n]  e.g. cch, return, cb+2

   If the buffer size is a constant expression use the c_ postfix.
   E.g. cap_c_(20), count_c_(MAX_PATH), bytecount_c_(16)

   If the buffer size is given by a limiting pointer use the ptrdiff_ versions
   of the macros.

   If the buffer size is neither a parameter nor a constant expression use the x_
   postfix. e.g. bytecount_x_(num*size) x_ annotations accept any arbitrary string.
   No analysis can be done for x_ annotations but they at least tell the tool that
   the buffer has some sort of extent description. x_ annotations might be supported
   by future compiler versions.

============================================================================*/

// e.g. void SetCharRange( _In_count_(cch) const char* rgch, size_t cch )
// valid buffer extent described by another parameter





// valid buffer extent described by a constant extression





// nullterminated  'input' buffers with given size

// e.g. void SetCharRange( _In_count_(cch) const char* rgch, size_t cch )
// nullterminated valid buffer extent described by another parameter





// nullterminated valid buffer extent described by a constant extression





// buffer capacity is described by another pointer
// e.g. void Foo( _In_ptrdiff_count_(pchMax) const char* pch, const char* pchMax ) { while pch < pchMax ) pch++; }



// 'x' version for complex expressions that are not supported by the current compiler version
// e.g. void Set3ColMatrix( _In_count_x_(3*cRows) const Elem* matrix, int cRows );






// 'out' with buffer size
// e.g. void GetIndices( _Out_cap_(cIndices) int* rgIndices, size_t cIndices );
// buffer capacity is described by another parameter





// buffer capacity is described by a constant expression





// buffer capacity is described by another parameter multiplied by a constant expression





// buffer capacity is described by another pointer
// e.g. void Foo( _Out_ptrdiff_cap_(pchMax) char* pch, const char* pchMax ) { while pch < pchMax ) pch++; }



// buffer capacity is described by a complex expression





// a zero terminated string is filled into a buffer of given capacity
// e.g. void CopyStr( _In_z_ const char* szFrom, _Out_z_cap_(cchTo) char* szTo, size_t cchTo );
// buffer capacity is described by another parameter





// buffer capacity is described by a constant expression





// buffer capacity is described by a complex expression





// a zero terminated string is filled into a buffer of given capacity
// e.g. size_t CopyCharRange( _In_count_(cchFrom) const char* rgchFrom, size_t cchFrom, _Out_cap_post_count_(cchTo,return)) char* rgchTo, size_t cchTo );





// a zero terminated string is filled into a buffer of given capacity
// e.g. size_t CopyStr( _In_z_ const char* szFrom, _Out_z_cap_post_count_(cchTo,return+1) char* szTo, size_t cchTo );





// only use with dereferenced arguments e.g. '*pcch'










// e.g. GetString( _Out_z_capcount_(*pLen+1) char* sz, size_t* pLen );






// 'inout' buffers with initialized elements before and after the call
// e.g. void ModifyIndices( _Inout_count_(cIndices) int* rgIndices, size_t cIndices );










// nullterminated 'inout' buffers with initialized elements before and after the call
// e.g. void ModifyIndices( _Inout_count_(cIndices) int* rgIndices, size_t cIndices );


















// e.g. void AppendToLPSTR( _In_ LPCSTR szFrom, _Inout_cap_(cchTo) LPSTR* szTo, size_t cchTo );















// inout string buffers with writable size
// e.g. void AppendStr( _In_z_ const char* szFrom, _Inout_z_cap_(cchTo) char* szTo, size_t cchTo );
















// returning pointers to valid objects



// annotations to express 'boundedness' of integral value parameter








// e.g.  HRESULT HrCreatePoint( _Deref_out_opt_ POINT** ppPT );





// e.g.  void CloneString( _In_z_ const wchar_t* wzFrom, _Deref_out_z_ wchar_t** pWzTo );





//
// _Deref_pre_ ---
//
// describing conditions for array elements of dereferenced pointer parameters that must be met before the call

// e.g. void SaveStringArray( _In_count_(cStrings) _Deref_pre_z_ const wchar_t* const rgpwch[] );



// e.g. void FillInArrayOfStr32( _In_count_(cStrings) _Deref_pre_cap_c_(32) _Deref_post_z_ wchar_t* const rgpwch[] );
// buffer capacity is described by another parameter





// buffer capacity is described by a constant expression





// buffer capacity is described by a complex condition





// convenience macros for nullterminated buffers with given capacity















// known capacity and valid but unknown readable extent















// e.g. void SaveMatrix( _In_count_(n) _Deref_pre_count_(n) const Elem** matrix, size_t n );
// valid buffer extent is described by another parameter





// valid buffer extent is described by a constant expression





// valid buffer extent is described by a complex expression





// e.g. void PrintStringArray( _In_count_(cElems) _Deref_pre_valid_ LPCSTR rgStr[], size_t cElems );








// restrict access rights



//
// _Deref_post_ ---
//
// describing conditions for array elements or dereferenced pointer parameters that hold after the call

// e.g. void CloneString( _In_z_ const Wchar_t* wzIn _Out_ _Deref_post_z_ wchar_t** pWzOut );



// e.g. HRESULT HrAllocateMemory( size_t cb, _Out_ _Deref_post_bytecap_(cb) void** ppv );
// buffer capacity is described by another parameter





// buffer capacity is described by a constant expression





// buffer capacity is described by a complex expression





// convenience macros for nullterminated buffers with given capacity















// known capacity and valid but unknown readable extent















// e.g. HRESULT HrAllocateZeroInitializedMemory( size_t cb, _Out_ _Deref_post_bytecount_(cb) void** ppv );
// valid buffer extent is described by another parameter





// buffer capacity is described by a constant expression





// buffer capacity is described by a complex expression





// e.g. void GetStrings( _Out_count_(cElems) _Deref_post_valid_ LPSTR const rgStr[], size_t cElems );







//
// _Deref_ret_ ---
//




//
// special _Deref_ ---
//


//
// _Ret_ ---
//

// e.g. _Ret_opt_valid_ LPSTR void* CloneSTR( _Pre_valid_ LPSTR src );



// e.g. _Ret_opt_bytecap_(cb) void* AllocateMemory( size_t cb );
// Buffer capacity is described by another parameter





// Buffer capacity is described by a constant expression





// Buffer capacity is described by a complex condition





// return value is nullterminated and capacity is given by another parameter





// e.g. _Ret_opt_bytecount_(cb) void* AllocateZeroInitializedMemory( size_t cb );
// Valid Buffer extent is described by another parameter





// Valid Buffer extent is described by a constant expression





// Valid Buffer extent is described by a complex expression





// return value is nullterminated and length is given by another parameter






// _Pre_ annotations ---


// restrict access rights



// e.g. void FreeMemory( _Pre_bytecap_(cb) _Post_ptr_invalid_ void* pv, size_t cb );
// buffer capacity described by another parameter





// buffer capacity described by a constant expression







// buffer capacity is described by another parameter multiplied by a constant expression



// buffer capacity described by size of other buffer, only used by dangerous legacy APIs
// e.g. int strcpy(_Pre_cap_for_(src) char* dst, const char* src);



// buffer capacity described by a complex condition





// buffer capacity described by the difference to another pointer parameter



// e.g. void AppendStr( _Pre_z_ const char* szFrom, _Pre_z_cap_(cchTo) _Post_z_ char* szTo, size_t cchTo );















// known capacity and valid but unknown readable extent















// e.g. void AppendCharRange( _Pre_count_(cchFrom) const char* rgFrom, size_t cchFrom, _Out_z_cap_(cchTo) char* szTo, size_t cchTo );
// Valid buffer extent described by another parameter





// Valid buffer extent described by a constant expression





// Valid buffer extent described by a complex expression





// Valid buffer extent described by the difference to another pointer parameter




// char * strncpy(_Out_cap_(_Count) _Post_maybez_ char * _Dest, _In_z_ const char * _Source, _In_ size_t _Count)
// buffer maybe zero-terminated after the call


// e.g. SIZE_T HeapSize( _In_ HANDLE hHeap, DWORD dwFlags, _Pre_notnull_ _Post_bytecap_(return) LPCVOID lpMem );



// e.g. int strlen( _In_z_ _Post_count_(return+1) const char* sz );







// e.g. size_t CopyStr( _In_z_ const char* szFrom, _Pre_cap_(cch) _Post_z_count_(return+1) char* szFrom, size_t cchFrom );







//
// _Prepost_ ---
//
// describing conditions that hold before and after the function call



















//
// _Deref_<both> ---
//
// short version for _Deref_pre_<ann> _Deref_post_<ann>
// describing conditions for array elements or dereferenced pointer parameters that hold before and after the call










































//
// _Deref_<miscellaneous>
//
// used with references to arrays







#pragma endregion Input Buffer SAL 1 compatibility macros


//============================================================================
//   Implementation Layer:
//============================================================================


// Naming conventions:
// A symbol the begins with _SA_ is for the machinery of creating any
// annotations; many of those come from sourceannotations.h in the case
// of attributes.

// A symbol that ends with _impl is the very lowest level macro.  It is
// not required to be a legal standalone annotation, and in the case
// of attribute annotations, usually is not.  (In the case of some declspec
// annotations, it might be, but it should not be assumed so.)  Those
// symbols will be used in the _PreN..., _PostN... and _RetN... annotations
// to build up more complete annotations.

// A symbol ending in _impl_ is reserved to the implementation as well,
// but it does form a complete annotation; usually they are used to build
// up even higher level annotations.



























































#line 1555 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






























#line 1586 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
























#line 1611 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

// Using "nothing" for sal










#line 1624 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






































#line 1663 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"















































































































#line 1775 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






































































































#line 1878 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"








































































































































































#line 2047 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

































































































// Obsolete -- may be needed for transition to attributes.



#line 2149 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

// This section contains the deprecated annotations

/*
 -------------------------------------------------------------------------------
 Introduction

 sal.h provides a set of annotations to describe how a function uses its
 parameters - the assumptions it makes about them, and the guarantees it makes
 upon finishing.

 Annotations may be placed before either a function parameter's type or its return
 type, and describe the function's behavior regarding the parameter or return value.
 There are two classes of annotations: buffer annotations and advanced annotations.
 Buffer annotations describe how functions use their pointer parameters, and
 advanced annotations either describe complex/unusual buffer behavior, or provide
 additional information about a parameter that is not otherwise expressible.

 -------------------------------------------------------------------------------
 Buffer Annotations

 The most important annotations in sal.h provide a consistent way to annotate
 buffer parameters or return values for a function. Each of these annotations describes
 a single buffer (which could be a string, a fixed-length or variable-length array,
 or just a pointer) that the function interacts with: where it is, how large it is,
 how much is initialized, and what the function does with it.

 The appropriate macro for a given buffer can be constructed using the table below.
 Just pick the appropriate values from each category, and combine them together
 with a leading underscore. Some combinations of values do not make sense as buffer
 annotations. Only meaningful annotations can be added to your code; for a list of
 these, see the buffer annotation definitions section.

 Only a single buffer annotation should be used for each parameter.

 |------------|------------|---------|--------|----------|----------|---------------|
 |   Level    |   Usage    |  Size   | Output | NullTerm | Optional |  Parameters   |
 |------------|------------|---------|--------|----------|----------|---------------|
 | <>         | <>         | <>      | <>     | _z       | <>       | <>            |
 | _deref     | _in        | _ecount | _full  | _nz      | _opt     | (size)        |
 | _deref_opt | _out       | _bcount | _part  |          |          | (size,length) |
 |            | _inout     |         |        |          |          |               |
 |            |            |         |        |          |          |               |
 |------------|------------|---------|--------|----------|----------|---------------|

 Level: Describes the buffer pointer's level of indirection from the parameter or
          return value 'p'.

 <>         : p is the buffer pointer.
 _deref     : *p is the buffer pointer. p must not be NULL.
 _deref_opt : *p may be the buffer pointer. p may be NULL, in which case the rest of
                the annotation is ignored.

 Usage: Describes how the function uses the buffer.

 <>     : The buffer is not accessed. If used on the return value or with _deref, the
            function will provide the buffer, and it will be uninitialized at exit.
            Otherwise, the caller must provide the buffer. This should only be used
            for alloc and free functions.
 _in    : The function will only read from the buffer. The caller must provide the
            buffer and initialize it. Cannot be used with _deref.
 _out   : The function will only write to the buffer. If used on the return value or
            with _deref, the function will provide the buffer and initialize it.
            Otherwise, the caller must provide the buffer, and the function will
            initialize it.
 _inout : The function may freely read from and write to the buffer. The caller must
            provide the buffer and initialize it. If used with _deref, the buffer may
            be reallocated by the function.

 Size: Describes the total size of the buffer. This may be less than the space actually
         allocated for the buffer, in which case it describes the accessible amount.

 <>      : No buffer size is given. If the type specifies the buffer size (such as
             with LPSTR and LPWSTR), that amount is used. Otherwise, the buffer is one
             element long. Must be used with _in, _out, or _inout.
 _ecount : The buffer size is an explicit element count.
 _bcount : The buffer size is an explicit byte count.

 Output: Describes how much of the buffer will be initialized by the function. For
           _inout buffers, this also describes how much is initialized at entry. Omit this
           category for _in buffers; they must be fully initialized by the caller.

 <>    : The type specifies how much is initialized. For instance, a function initializing
           an LPWSTR must NULL-terminate the string.
 _full : The function initializes the entire buffer.
 _part : The function initializes part of the buffer, and explicitly indicates how much.

 NullTerm: States if the present of a '\0' marks the end of valid elements in the buffer.
 _z    : A '\0' indicated the end of the buffer
 _nz     : The buffer may not be null terminated and a '\0' does not indicate the end of the
          buffer.
 Optional: Describes if the buffer itself is optional.

 <>   : The pointer to the buffer must not be NULL.
 _opt : The pointer to the buffer might be NULL. It will be checked before being dereferenced.

 Parameters: Gives explicit counts for the size and length of the buffer.

 <>            : There is no explicit count. Use when neither _ecount nor _bcount is used.
 (size)        : Only the buffer's total size is given. Use with _ecount or _bcount but not _part.
 (size,length) : The buffer's total size and initialized length are given. Use with _ecount_part
                   and _bcount_part.

 -------------------------------------------------------------------------------
 Buffer Annotation Examples

 LWSTDAPI_(BOOL) StrToIntExA(
     __in LPCSTR pszString,
     DWORD dwFlags,
     __out int *piRet                     -- A pointer whose dereference will be filled in.
 );

 void MyPaintingFunction(
     __in HWND hwndControl,               -- An initialized read-only parameter.
     __in_opt HDC hdcOptional,            -- An initialized read-only parameter that might be NULL.
     __inout IPropertyStore *ppsStore     -- An initialized parameter that may be freely used
                                          --   and modified.
 );

 LWSTDAPI_(BOOL) PathCompactPathExA(
     __out_ecount(cchMax) LPSTR pszOut,   -- A string buffer with cch elements that will
                                          --   be NULL terminated on exit.
     __in LPCSTR pszSrc,
     UINT cchMax,
     DWORD dwFlags
 );

 HRESULT SHLocalAllocBytes(
     size_t cb,
     __deref_bcount(cb) T **ppv           -- A pointer whose dereference will be set to an
                                          --   uninitialized buffer with cb bytes.
 );

 __inout_bcount_full(cb) : A buffer with cb elements that is fully initialized at
     entry and exit, and may be written to by this function.

 __out_ecount_part(count, *countOut) : A buffer with count elements that will be
     partially initialized by this function. The function indicates how much it
     initialized by setting *countOut.

 -------------------------------------------------------------------------------
 Advanced Annotations

 Advanced annotations describe behavior that is not expressible with the regular
 buffer macros. These may be used either to annotate buffer parameters that involve
 complex or conditional behavior, or to enrich existing annotations with additional
 information.

 __success(expr) f :
     <expr> indicates whether function f succeeded or not. If <expr> is true at exit,
     all the function's guarantees (as given by other annotations) must hold. If <expr>
     is false at exit, the caller should not expect any of the function's guarantees
     to hold. If not used, the function must always satisfy its guarantees. Added
     automatically to functions that indicate success in standard ways, such as by
     returning an HRESULT.

 __nullterminated p :
     Pointer p is a buffer that may be read or written up to and including the first
     NULL character or pointer. May be used on typedefs, which marks valid (properly
     initialized) instances of that type as being NULL-terminated.

 __nullnullterminated p :
     Pointer p is a buffer that may be read or written up to and including the first
     sequence of two NULL characters or pointers. May be used on typedefs, which marks
     valid instances of that type as being double-NULL terminated.

 __reserved v :
     Value v must be 0/NULL, reserved for future use.

 __checkReturn v :
     Return value v must not be ignored by callers of this function.

 __typefix(ctype) v :
     Value v should be treated as an instance of ctype, rather than its declared type.

 __override f :
     Specify C#-style 'override' behaviour for overriding virtual methods.

 __callback f :
     Function f can be used as a function pointer.

 __format_string p :
     Pointer p is a string that contains % markers in the style of printf.

 __blocksOn(resource) f :
     Function f blocks on the resource 'resource'.

 __fallthrough :
     Annotates switch statement labels where fall-through is desired, to distinguish
     from forgotten break statements.

 -------------------------------------------------------------------------------
 Advanced Annotation Examples

 __success(return != FALSE) LWSTDAPI_(BOOL)
 PathCanonicalizeA(__out_ecount(MAX_PATH) LPSTR pszBuf, LPCSTR pszPath) :
    pszBuf is only guaranteed to be NULL-terminated when TRUE is returned.

 typedef __nullterminated WCHAR* LPWSTR : Initialized LPWSTRs are NULL-terminated strings.

 __out_ecount(cch) __typefix(LPWSTR) void *psz : psz is a buffer parameter which will be
     a NULL-terminated WCHAR string at exit, and which initially contains cch WCHARs.

 -------------------------------------------------------------------------------
*/











#line 2366 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
#line 2367 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"


/*
 -------------------------------------------------------------------------------
 Helper Macro Definitions

 These express behavior common to many of the high-level annotations.
 DO NOT USE THESE IN YOUR CODE.
 -------------------------------------------------------------------------------
*/

/*
    The helper annotations are only understood by the compiler version used by
    various defect detection tools. When the regular compiler is running, they
    are defined into nothing, and do not affect the compiled code.
*/



















































































































































































































#line 2595 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    
    
    
    

    
    

#line 2634 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

/*
-------------------------------------------------------------------------------
Buffer Annotation Definitions

Any of these may be used to directly annotate functions, but only one should
be used for each parameter. To determine which annotation to use for a given
buffer, use the table in the buffer annotations section.
-------------------------------------------------------------------------------
*/
































































































































































































/*
-------------------------------------------------------------------------------
Advanced Annotation Definitions

Any of these may be used to directly annotate functions, and may be used in
combination with each other or with regular buffer macros. For an explanation
of each annotation, see the advanced annotations section.
-------------------------------------------------------------------------------
*/






















#line 2868 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"









#line 2878 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"



    
    


#line 2886 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
#line 2887 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






#line 2894 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
#line 2895 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"






#line 2902 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
#line 2903 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"











#line 2915 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

//
// Set the analysis mode (global flags to analysis).
// They take effect at the point of declaration; use at global scope
// as a declaration.
//

// Synthesize a unique symbol.








//
// Floating point warnings are only meaningful in kernel-mode on x86
// so avoid reporting them on other platforms.
//













#line 2949 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"

// The following are predefined:
//  _Analysis_operator_new_throw_   (operator new throws)
//  _Analysis_operator_new_null_        (operator new returns null)
//  _Analysis_operator_new_never_fails_ (operator new never fails)
//

// Function class annotations.

















#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\concurrencysal.h"
/***
*concurrencysal.h - markers for documenting the concurrent semantics of APIs
*
*       Copyright (c) Microsoft Corporation. All rights reserved.
*
*Purpose:
*       This file contains macros for Concurrency SAL annotations. Definitions
*       starting with _Internal are low level macros that are subject to change.
*       Users should not use those low level macros directly.
*       [ANSI]
*
*       [Public]
*
****/




#pragma once















































































































































































































































































#line 292 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\concurrencysal.h"



#line 296 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\concurrencysal.h"























































/*
 * Old spelling: will be deprecated
 */


































#line 389 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\concurrencysal.h"





#line 395 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\concurrencysal.h"
#line 2975 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\sal.h"
#line 58 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"
//
// vadefs.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Definitions of macro helpers used by <stdarg.h>.  This is the topmost header
// in the CRT header lattice, and is always the first CRT header to be included,
// explicitly or implicitly.  Therefore, this header also has several definitions
// that are used throughout the CRT.
//
#pragma once



#pragma pack(push, 8)

// C4339: '__type_info_node': use of undefined type detected in CLR meta-data (/Wall)

    


        
    #line 24 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"
#line 25 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

// C4412: function signature contains type '<typename>';
//        C++ objects are unsafe to pass between pure code and mixed or native. (/Wall)

    


        
    #line 34 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"
#line 35 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

// Use _VCRUNTIME_EXTRA_DISABLED_WARNINGS to add additional warning suppressions to VCRuntime headers.

    
#line 40 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

// C4514: unreferenced inline function has been removed (/Wall)
// C4820: '<typename>' : 'N' bytes padding added after data member (/Wall)

    
#line 46 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

#pragma warning(push)
#pragma warning(disable:   4514 4820 )







#line 57 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"


    
    
        typedef unsigned __int64  uintptr_t;
    

#line 65 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"
#line 66 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"


    
    


        typedef char* va_list;
    #line 74 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"
#line 75 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"




    
#line 81 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"





#line 87 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"



#line 91 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"
    
    
#line 94 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"











#line 106 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"







#line 114 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"











#line 126 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"





#line 132 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"










#line 143 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"










#line 154 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

    void __cdecl __va_start(va_list* , ...);

    
    



    

#line 165 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"




































#line 202 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

    

#line 206 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vadefs.h"

#pragma warning(pop) 
#pragma pack(pop)
#line 59 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

#pragma warning(push)
#pragma warning(disable:   4514 4820 )

// All C headers have a common prologue and epilogue, to enclose the header in
// an extern "C" declaration when the header is #included in a C++ translation
// unit and to push/pop the packing.










#line 77 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"









#line 87 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

    


    


#line 95 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

__pragma(pack(push, 8))




    


        
    #line 106 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 107 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
















    

#line 126 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

#line 128 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        
    #line 130 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 131 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    

#line 136 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        
    #line 138 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 139 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// Definitions of calling conventions used code sometimes compiled as managed



#line 145 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
    
    
#line 148 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"




    
#line 154 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



// Definitions of common __declspecs






#line 165 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
    
#line 167 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"




    
#line 173 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    



      
    #line 181 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 182 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// For backwards compatibility


// Definitions of common types

    typedef unsigned __int64 size_t;
    typedef __int64          ptrdiff_t;
    typedef __int64          intptr_t;




#line 196 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 200 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 204 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
    typedef _Bool __vcrt_bool;
#line 206 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// Indicate that these common types are defined

    
#line 211 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    
#line 215 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    
#line 219 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// Provide a typedef for wchar_t for use under /Zc:wchar_t-

    
    typedef unsigned short wchar_t;
#line 225 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    


        
    #line 232 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 233 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    


#line 239 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 243 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"










    
#line 255 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 259 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"








    
#line 269 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    

#line 274 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        
    #line 276 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 277 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    

#line 282 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        
    #line 284 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 285 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    

#line 290 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        
    #line 292 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 293 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    

#line 298 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        
    #line 300 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 301 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"





#line 307 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 311 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 315 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// [[nodiscard]] attributes on STL functions

    
        
    



#line 325 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 326 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 330 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
    
#line 332 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"






// Determine if we should use [[msvc::constexpr]] to allow for "extended constexpr"
// in Visual C++.

    


        
    #line 346 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 347 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"







    
#line 356 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

// See note on use of "deprecate" at the top of this file




#line 363 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    


        




    #line 374 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 375 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"



#line 379 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    
        
    


#line 387 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 388 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


    void __cdecl __security_init_cookie(void);

    


#line 396 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"


#line 399 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
        void __cdecl __security_check_cookie(  uintptr_t _StackCookie);
        __declspec(noreturn) void __cdecl __report_gsfailure(  uintptr_t _StackCookie);
    #line 402 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 403 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

extern uintptr_t __security_cookie;


    
    
    
#line 411 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"

__pragma(pack(pop))

#pragma warning(pop) 

#line 417 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime.h"
#line 11 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Windows API Partitioning and ARM Desktop Support
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    

















        
    #line 38 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 39 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    
#line 43 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    
        
    

#line 50 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 51 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// Verify that the ARM Desktop SDK is available when building an ARM Desktop app






//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Warning Suppression
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

// C4412: function signature contains type '_locale_t';
//        C++ objects are unsafe to pass between pure code and mixed or native. (/Wall)

    


        
    #line 73 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 74 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// Use _UCRT_EXTRA_DISABLED_WARNINGS to add additional warning suppressions to UCRT headers.

    
#line 79 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// C4324: structure was padded due to __declspec(align()) (/W4)
// C4514: unreferenced inline function has been removed (/Wall)
// C4574: 'MACRO' is defined to be '0': did you mean to use '#if MACRO'? (/Wall)
// C4668: '__cplusplus' is not defined as a preprocessor macro (/Wall)
// C4710: function not inlined (/Wall)
// C4793: 'function' is compiled as native code (/Wall and /W1 under /clr:pure)
// C4820: padding after data member (/Wall)
// C4995: name was marked #pragma deprecated
// C4996: __declspec(deprecated)
// C28719: Banned API, use a more robust and secure replacement.
// C28726: Banned or deprecated API, use a more robust and secure replacement.
// C28727: Banned API.

    
#line 95 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    











        
    #line 111 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 112 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        
    #line 119 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 120 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))

//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Annotation Macros
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    

#line 136 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

#line 138 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
        
    #line 140 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 141 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// If you need the ability to remove __declspec(import) from an API, to support static replacement,
// declare the API using _ACRTIMP_ALT instead of _ACRTIMP.

    
#line 147 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    

#line 152 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

#line 154 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
        
    #line 156 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 157 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 161 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    
#line 163 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


#line 169 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"





#line 175 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    
#line 177 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// __declspec(guard(overflow)) enabled by /sdl compiler switch for CRT allocators



    
#line 184 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 188 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    
#line 190 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// The CLR requires code calling other SecurityCritical code or using SecurityCritical types
// to be marked as SecurityCritical.
// _CRT_SECURITYCRITICAL_ATTRIBUTE covers this for internal function definitions.
// _CRT_INLINE_PURE_SECURITYCRITICAL_ATTRIBUTE is for inline pure functions defined in the header.
// This is clr:pure-only because for mixed mode we compile inline functions as native.



    
#line 201 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"













    


        
    #line 219 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 220 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 224 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    
#line 226 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 230 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    
#line 232 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 236 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    
#line 238 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        
    #line 245 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 246 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Miscellaneous Stuff
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
















#line 270 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 274 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    typedef _Bool __crt_bool;
#line 276 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"














    
        


            
        #line 296 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    #line 297 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 298 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"









// CRT headers are included into some kinds of source files where only data type
// definitions and macro definitions are required but function declarations and
// inline function definitions are not.  These files include assembly files, IDL
// files, and resource files.  The tools that process these files often have a
// limited ability to process C and C++ code.  The _CRT_FUNCTIONS_REQUIRED macro
// is defined to 1 when we are compiling a file that actually needs functions to
// be declared (and defined, where applicable), and to 0 when we are compiling a
// file that does not.  This allows us to suppress declarations and definitions
// that are not compilable with the aforementioned tools.

    

#line 320 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
        
    #line 322 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 323 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 327 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    
#line 331 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


 






  

#line 343 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
   
  #line 345 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
 #line 346 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 347 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


 

#line 352 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
   
 #line 354 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 355 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Invalid Parameter Handler
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+










 void __cdecl _invalid_parameter_noinfo(void);
 __declspec(noreturn) void __cdecl _invalid_parameter_noinfo_noreturn(void);

__declspec(noreturn)
 void __cdecl _invoke_watson(
      wchar_t const* _Expression,
      wchar_t const* _FunctionName,
      wchar_t const* _FileName,
            unsigned int _LineNo,
            uintptr_t _Reserved);


    



        // By default, _CRT_SECURE_INVALID_PARAMETER in retail invokes
        // _invalid_parameter_noinfo_noreturn(), which is marked
        // __declspec(noreturn) and does not return control to the application.
        // Even if _set_invalid_parameter_handler() is used to set a new invalid
        // parameter handler which does return control to the application,
        // _invalid_parameter_noinfo_noreturn() will terminate the application
        // and invoke Watson. You can overwrite the definition of
        // _CRT_SECURE_INVALID_PARAMETER if you need.
        //
        // _CRT_SECURE_INVALID_PARAMETER is used in the Standard C++ Libraries
        // and the SafeInt library.
        

    #line 401 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 402 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Deprecation and Warnings
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+




    


#line 419 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 423 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        


    #line 432 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 433 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Managed CRT Support
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    






        
    #line 451 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 452 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        
    #line 459 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 460 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 464 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// SecureCRT Configuration
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+





#line 478 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"















#line 494 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"





    
#line 501 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 505 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    

#line 510 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 511 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        


            
        #line 521 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    #line 522 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 523 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 527 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"





#line 533 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        



    #line 543 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 544 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    
        
    



#line 553 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

    
        // _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES_COUNT is ignored if
        // _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES is set to 0
        
    



#line 563 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

    
        
              
        

#line 570 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    



#line 575 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

    
        
    



#line 583 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

    
        
    



#line 591 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 592 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    
#line 596 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Basic Types
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
typedef int                           errno_t;
typedef unsigned short                wint_t;
typedef unsigned short                wctype_t;
typedef long                          __time32_t;
typedef __int64                       __time64_t;

typedef struct __crt_locale_data_public
{
      unsigned short const* _locale_pctype;
      int _locale_mb_cur_max;
               unsigned int _locale_lc_codepage;
} __crt_locale_data_public;

typedef struct __crt_locale_pointers
{
    struct __crt_locale_data*    locinfo;
    struct __crt_multibyte_data* mbcinfo;
} __crt_locale_pointers;

typedef __crt_locale_pointers* _locale_t;

typedef struct _Mbstatet
{ // state of a multibyte translation
    unsigned long _Wchar;
    unsigned short _Byte, _State;
} _Mbstatet;

typedef _Mbstatet mbstate_t;



#line 636 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"



#line 640 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    


        typedef __time64_t time_t;
    #line 647 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 648 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

// Indicate that these common types are defined

    
#line 653 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"


    typedef size_t rsize_t;
#line 657 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"




//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// C++ Secure Overload Generation Macros
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    















































































































































#line 812 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

        
        
        
        
        
        
        
        
        
        
        
        

    #line 827 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 828 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"






































































//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// C++ Standard Overload Generation Macros
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    













































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































#line 1879 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

        
        
        
        

        

            


            


            


            


            


            


            


            


            



            



            


            


            


            


            


            


            


            


            


            


            



            



            



            


            



            




            

            




            

            




            

            




            

            




            

            




            

            




            

            




            

        











































#line 2069 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
    #line 2070 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"
#line 2071 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt.h"

__pragma(pack(pop))


#pragma warning(pop) 
#line 13 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_malloc.h"
//
// corecrt_malloc.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The memory allocation library.  These pieces of the allocation library are
// shared by both <stdlib.h> and <malloc.h>.
//
#pragma once



#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))





































#line 56 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_malloc.h"

     
 __declspec(allocator) __declspec(restrict)
void* __cdecl _calloc_base(
      size_t _Count,
      size_t _Size
    );

     
  __declspec(allocator) __declspec(restrict) 
void* __cdecl calloc(
       size_t _Count,
       size_t _Size
    );

 
 int __cdecl _callnewh(
      size_t _Size
    );

     
 __declspec(allocator) 
void* __cdecl _expand(
                void*  _Block,
       size_t _Size
    );


void __cdecl _free_base(
        void* _Block
    );

 
void __cdecl free(
        void* _Block
    );

     
 __declspec(allocator) __declspec(restrict)
void* __cdecl _malloc_base(
      size_t _Size
    );

     
 __declspec(allocator)  __declspec(restrict) 
void* __cdecl malloc(
       size_t _Size
    );

 

size_t __cdecl _msize_base(
      void* _Block
    ) ;

 
 
size_t __cdecl _msize(
      void* _Block
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _realloc_base(
         void*  _Block,
                                 size_t _Size
    );

       
 __declspec(allocator) __declspec(restrict) 
void* __cdecl realloc(
        void*  _Block,
              size_t _Size
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _recalloc_base(
        void*  _Block,
                                size_t _Count,
                                size_t _Size
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _recalloc(
        void*  _Block,
              size_t _Count,
              size_t _Size
    );


void __cdecl _aligned_free(
        void* _Block
    );

     
 __declspec(allocator) __declspec(restrict)
void* __cdecl _aligned_malloc(
       size_t _Size,
                         size_t _Alignment
    );

     
 __declspec(allocator) __declspec(restrict)
void* __cdecl _aligned_offset_malloc(
       size_t _Size,
                         size_t _Alignment,
                         size_t _Offset
    );

 

size_t __cdecl _aligned_msize(
      void*  _Block,
               size_t _Alignment,
               size_t _Offset
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _aligned_offset_realloc(
        void*  _Block,
              size_t _Size,
                                size_t _Alignment,
                                size_t _Offset
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _aligned_offset_recalloc(
        void*  _Block,
              size_t _Count,
              size_t _Size,
                                size_t _Alignment,
                                size_t _Offset
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _aligned_realloc(
        void*  _Block,
              size_t _Size,
                                size_t _Alignment
    );

       
 __declspec(allocator) __declspec(restrict)
void* __cdecl _aligned_recalloc(
        void*  _Block,
              size_t _Count,
              size_t _Size,
                                size_t _Alignment
    );


















#line 229 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_malloc.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 14 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_search.h"
//
// corecrt_search.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Declarations of functions for sorting and searching.  These declarations are
// split out so that they may be included by both <stdlib.h> and <search.h>.
// <stdlib.h> does not include <search.h> to avoid introducing conflicts with
// other user headers named <search.h>.
//
#pragma once


#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stddef.h"
//
// stddef.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C <stddef.h> Standard Library header.
//
#pragma once





#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))
















     int* __cdecl _errno(void);
    

     errno_t __cdecl _set_errno(  int _Value);
     errno_t __cdecl _get_errno(  int* _Value);

#line 42 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stddef.h"




    


        
    #line 51 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stddef.h"


#line 54 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stddef.h"

 extern unsigned long  __cdecl __threadid(void);

 extern uintptr_t __cdecl __threadhandle(void);



__pragma(pack(pop))

#pragma warning(pop) 
#line 65 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stddef.h"
#line 15 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_search.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))


    typedef int (__cdecl* _CoreCrtSecureSearchSortCompareFunction)(void*, void const*, void const*);
    typedef int (__cdecl* _CoreCrtNonSecureSearchSortCompareFunction)(void const*, void const*);




     
     void* __cdecl bsearch_s(
                                                        void const* _Key,
          void const* _Base,
                                                        rsize_t     _NumOfElements,
                                                        rsize_t     _SizeOfElements,
                            _CoreCrtSecureSearchSortCompareFunction _CompareFunction,
                                                    void*       _Context
        );

     void __cdecl qsort_s(
          void*   _Base,
                                                             rsize_t _NumOfElements,
                                                             rsize_t _SizeOfElements,
                             _CoreCrtSecureSearchSortCompareFunction _CompareFunction,
                                                         void*   _Context
        );

#line 48 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_search.h"



 
 void* __cdecl bsearch(
                                                    void const* _Key,
      void const* _Base,
                                                    size_t      _NumOfElements,
                                                    size_t      _SizeOfElements,
                     _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction
    );

 void __cdecl qsort(
      void*  _Base,
                                                         size_t _NumOfElements,
                                                         size_t _SizeOfElements,
                     _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction
    );

 
 void* __cdecl _lfind_s(
                                                       void const*   _Key,
      void const*   _Base,
                                                    unsigned int* _NumOfElements,
                                                       size_t        _SizeOfElements,
                             _CoreCrtSecureSearchSortCompareFunction _CompareFunction,
                                                       void*         _Context
    );

 
 void* __cdecl _lfind(
                                                       void const*   _Key,
      void const*   _Base,
                                                    unsigned int* _NumOfElements,
                                                       unsigned int  _SizeOfElements,
                          _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction
    );

 
 void* __cdecl _lsearch_s(
                                   void const*   _Key,
      void*         _Base,
                                                             unsigned int* _NumOfElements,
                                                                size_t        _SizeOfElements,
                                      _CoreCrtSecureSearchSortCompareFunction _CompareFunction,
                                                                void*         _Context
    );

 
 void* __cdecl _lsearch(
                                   void const*   _Key,
      void*         _Base,
                                                             unsigned int* _NumOfElements,
                                                                unsigned int  _SizeOfElements,
                                   _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction
    );



// Managed search routines
















































































#line 189 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_search.h"





      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_lfind" ". See online help for details."))
     void* __cdecl lfind(
                                                           void const*   _Key,
          void const*   _Base,
                                                        unsigned int* _NumOfElements,
                                                           unsigned int  _SizeOfElements,
                              _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_lsearch" ". See online help for details."))
     void* __cdecl lsearch(
                                                                void const*   _Key,
          void*         _Base,
                                                             unsigned int* _NumOfElements,
                                                                unsigned int  _SizeOfElements,
                                   _CoreCrtNonSecureSearchSortCompareFunction _CompareFunction
        );

#line 213 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_search.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 15 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"
//
// corecrt_wstdlib.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// This file declares the wide character (wchar_t) C Standard Library functions
// that are declared by both <stdlib.h> and <wchar.h>.
//
#pragma once



#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))



// Maximum number of elements, including null terminator (and negative sign
// where appropriate), needed for integer-to-string conversions for several
// bases and integer types.




























     
    
     errno_t __cdecl _itow_s(
                                  int      _Value,
          wchar_t* _Buffer,
                                  size_t   _BufferCount,
                                  int      _Radix
        );

    
#line 67 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_itow_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _itow( int _Value,   wchar_t *_Buffer,  int _Radix);
#line 74 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

     
    
     errno_t __cdecl _ltow_s(
                                  long     _Value,
          wchar_t* _Buffer,
                                  size_t   _BufferCount,
                                  int      _Radix
        );

    
#line 90 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ltow_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _ltow( long _Value,   wchar_t *_Buffer,  int _Radix);
#line 97 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

    
     errno_t __cdecl _ultow_s(
                                  unsigned long _Value,
          wchar_t*      _Buffer,
                                  size_t        _BufferCount,
                                  int           _Radix
        );

    
#line 112 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ultow_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _ultow( unsigned long _Value,   wchar_t *_Buffer,  int _Radix);
#line 119 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

     
     double __cdecl wcstod(
                            wchar_t const* _String,
            wchar_t**      _EndPtr
        );

     
     double __cdecl _wcstod_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                          _locale_t      _Locale
        );

     
     long __cdecl wcstol(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix
        );

     
     long __cdecl _wcstol_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix,
                          _locale_t      _Locale
        );

     
     long long __cdecl wcstoll(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix
        );

     
     long long __cdecl _wcstoll_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix,
                          _locale_t      _Locale
        );

     
     unsigned long __cdecl wcstoul(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix
        );

     
     unsigned long __cdecl _wcstoul_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix,
                          _locale_t      _Locale
        );

     
     unsigned long long __cdecl wcstoull(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix
        );

     
     unsigned long long __cdecl _wcstoull_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix,
                          _locale_t      _Locale
        );

     
     long double __cdecl wcstold(
                            wchar_t const* _String,
            wchar_t**      _EndPtr
        );

     
     long double __cdecl _wcstold_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                          _locale_t      _Locale
        );

     
     float __cdecl wcstof(
                            wchar_t const* _String,
            wchar_t**      _EndPtr
        );

     
     float __cdecl _wcstof_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                          _locale_t      _Locale
        );

     
     double __cdecl _wtof(
          wchar_t const* _String
        );

     
     double __cdecl _wtof_l(
            wchar_t const* _String,
          _locale_t      _Locale
        );

     
     int __cdecl _wtoi(
          wchar_t const* _String
        );

     
     int __cdecl _wtoi_l(
            wchar_t const* _String,
          _locale_t      _Locale
        );

     
     long __cdecl _wtol(
          wchar_t const* _String
        );

     
     long __cdecl _wtol_l(
            wchar_t const* _String,
          _locale_t      _Locale
        );

     
     long long __cdecl _wtoll(
          wchar_t const* _String
        );

     
     long long __cdecl _wtoll_l(
            wchar_t const* _String,
          _locale_t      _Locale
        );

    
     errno_t __cdecl _i64tow_s(
                                  __int64  _Value,
          wchar_t* _Buffer,
                                  size_t   _BufferCount,
                                  int      _Radix
        );

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_i64tow_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     wchar_t* __cdecl _i64tow(
                            __int64  _Value,
            wchar_t* _Buffer,
                            int      _Radix
        );

    
     errno_t __cdecl _ui64tow_s(
                                  unsigned __int64 _Value,
          wchar_t*         _Buffer,
                                  size_t           _BufferCount,
                                  int              _Radix
        );

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ui64tow_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     wchar_t* __cdecl _ui64tow(
                            unsigned __int64 _Value,
            wchar_t*         _Buffer,
                            int              _Radix
        );

     
     __int64 __cdecl _wtoi64(
          wchar_t const* _String
        );

     
     __int64 __cdecl _wtoi64_l(
            wchar_t const* _String,
          _locale_t      _Locale
        );

     
     __int64 __cdecl _wcstoi64(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix
        );

     
     __int64 __cdecl _wcstoi64_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix,
                          _locale_t      _Locale
        );

     
     unsigned __int64 __cdecl _wcstoui64(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix
        );

     
     unsigned __int64 __cdecl _wcstoui64_l(
                            wchar_t const* _String,
            wchar_t**      _EndPtr,
                              int            _Radix,
                          _locale_t      _Locale
        );

    
    

     
     
     __declspec(allocator) wchar_t* __cdecl _wfullpath(
          wchar_t*       _Buffer,
                                    wchar_t const* _Path,
                                      size_t         _BufferCount
        );

    

    
     errno_t __cdecl _wmakepath_s(
          wchar_t*       _Buffer,
                                  size_t         _BufferCount,
                            wchar_t const* _Drive,
                            wchar_t const* _Dir,
                            wchar_t const* _Filename,
                            wchar_t const* _Ext
        );

    
#line 365 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wmakepath_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  void __cdecl _wmakepath(  wchar_t *_Buffer,  wchar_t const* _Drive,  wchar_t const* _Dir,  wchar_t const* _Filename,  wchar_t const* _Ext);
#line 374 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

     void __cdecl _wperror(
          wchar_t const* _ErrorMessage
        );

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wsplitpath_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     void __cdecl _wsplitpath(
                            wchar_t const* _FullPath,
            wchar_t*       _Drive,
            wchar_t*       _Dir,
            wchar_t*       _Filename,
            wchar_t*       _Ext
        );

     errno_t __cdecl _wsplitpath_s(
                                      wchar_t const* _FullPath,
             wchar_t*       _Drive,
                                        size_t         _DriveCount,
               wchar_t*       _Dir,
                                        size_t         _DirCount,
          wchar_t*       _Filename,
                                        size_t         _FilenameCount,
               wchar_t*       _Ext,
                                        size_t         _ExtCount
        );

    
#line 404 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

        
        

        
         errno_t __cdecl _wdupenv_s(
                wchar_t**      _Buffer,
                                                                                size_t*        _BufferCount,
                                                                                   wchar_t const* _VarName
            );

        

          __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wdupenv_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
         wchar_t* __cdecl _wgetenv(
              wchar_t const* _VarName
            );

         
        
         errno_t __cdecl _wgetenv_s(
                                         size_t*        _RequiredCount,
              wchar_t*       _Buffer,
                                          size_t         _BufferCount,
                                        wchar_t const* _VarName
            );

        
#line 438 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

         
         int __cdecl _wputenv(
              wchar_t const* _EnvString
            );

        
         errno_t __cdecl _wputenv_s(
              wchar_t const* _Name,
              wchar_t const* _Value
            );

         errno_t __cdecl _wsearchenv_s(
                                    wchar_t const* _Filename,
                                    wchar_t const* _VarName,
              wchar_t*       _Buffer,
                                      size_t         _BufferCount
            );

        
#line 463 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

        __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wsearchenv_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  void __cdecl _wsearchenv( wchar_t const* _Filename,  wchar_t const* _VarName,   wchar_t *_ResultPath);
#line 470 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"

         int __cdecl _wsystem(
              wchar_t const* _Command
            );

#line 476 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdlib.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 16 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\limits.h"
//
// limits.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <limits.h> header.
//
#pragma once




#pragma warning(push)
#pragma warning(disable:   4514 4820 )

__pragma(pack(push, 8))







    
    



#line 30 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\limits.h"
































    // SIZE_MAX definition must match exactly with stdint.h for modules support.
    
        
    

#line 68 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\limits.h"
#line 69 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\limits.h"


    
#line 73 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\limits.h"

__pragma(pack(pop))

#pragma warning(pop) 
#line 17 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))




    
#line 29 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



// Minimum and maximum macros





 void __cdecl _swab(
        char* _Buf1,
        char* _Buf2,
                                                                  int   _SizeInBytes
    );



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Exit and Abort
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// Argument values for exit()




     __declspec(noreturn) void __cdecl exit(  int _Code);
     __declspec(noreturn) void __cdecl _exit(  int _Code);
     __declspec(noreturn) void __cdecl _Exit(  int _Code);
     __declspec(noreturn) void __cdecl quick_exit(  int _Code);
     __declspec(noreturn) void __cdecl abort(void);
#line 62 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

// Argument values for _set_abort_behavior().



 unsigned int __cdecl _set_abort_behavior(
      unsigned int _Flags,
      unsigned int _Mask
    );




    

    typedef int (__cdecl* _onexit_t)(void);
    


#line 82 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"


    // Non-ANSI name for compatibility
    
#line 87 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

























































    int       __cdecl atexit(void (__cdecl*)(void));
    _onexit_t __cdecl _onexit(  _onexit_t _Func);
#line 147 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

int __cdecl at_quick_exit(void (__cdecl*)(void));



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Global State (errno, global handlers, etc.)
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    // a purecall handler procedure. Never returns normally
    typedef void (__cdecl* _purecall_handler)(void);

    // Invalid parameter handler function pointer type
    typedef void (__cdecl* _invalid_parameter_handler)(
        wchar_t const*,
        wchar_t const*,
        wchar_t const*,
        unsigned int,
        uintptr_t
        );

    // Establishes a purecall handler
     _purecall_handler __cdecl _set_purecall_handler(
          _purecall_handler _Handler
        );

     _purecall_handler __cdecl _get_purecall_handler(void);

    // Establishes an invalid parameter handler
     _invalid_parameter_handler __cdecl _set_invalid_parameter_handler(
          _invalid_parameter_handler _Handler
        );

     _invalid_parameter_handler __cdecl _get_invalid_parameter_handler(void);

     _invalid_parameter_handler __cdecl _set_thread_local_invalid_parameter_handler(
          _invalid_parameter_handler _Handler
        );

     _invalid_parameter_handler __cdecl _get_thread_local_invalid_parameter_handler(void);
#line 190 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"












#line 203 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



// Argument values for _set_error_mode().





  int __cdecl _set_error_mode(  int _Mode);




     int* __cdecl _errno(void);
    

     errno_t __cdecl _set_errno(  int _Value);
     errno_t __cdecl _get_errno(  int* _Value);

     unsigned long* __cdecl __doserrno(void);
    

     errno_t __cdecl _set_doserrno(  unsigned long _Value);
     errno_t __cdecl _get_doserrno(  unsigned long * _Value);

    // This is non-const for backwards compatibility; do not modify it.
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "strerror" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) char** __cdecl __sys_errlist(void);
    

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "strerror" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) int * __cdecl __sys_nerr(void);
    

     void __cdecl perror(  char const* _ErrMsg);
#line 238 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



// These point to the executable module name.
__declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_pgmptr" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char**    __cdecl __p__pgmptr (void);
__declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_wpgmptr" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t** __cdecl __p__wpgmptr(void);
__declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_fmode" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  int*      __cdecl __p__fmode  (void);








    
    
    
#line 257 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

 
 errno_t __cdecl _get_pgmptr (  char**    _Value);

 
 errno_t __cdecl _get_wpgmptr(  wchar_t** _Value);

 errno_t __cdecl _set_fmode  (               int       _Mode );

 errno_t __cdecl _get_fmode  (              int*      _PMode);



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Math
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
typedef struct _div_t
{
    int quot;
    int rem;
} div_t;

typedef struct _ldiv_t
{
    long quot;
    long rem;
} ldiv_t;

typedef struct _lldiv_t
{
    long long quot;
    long long rem;
} lldiv_t;

  int       __cdecl abs   (  int       _Number);
  long      __cdecl labs  (  long      _Number);
  long long __cdecl llabs (  long long _Number);
  __int64   __cdecl _abs64(  __int64   _Number);

  unsigned short   __cdecl _byteswap_ushort(  unsigned short   _Number);
  unsigned long    __cdecl _byteswap_ulong (  unsigned long    _Number);
  unsigned __int64 __cdecl _byteswap_uint64(  unsigned __int64 _Number);

   div_t   __cdecl div  (  int       _Numerator,   int       _Denominator);
   ldiv_t  __cdecl ldiv (  long      _Numerator,   long      _Denominator);
   lldiv_t __cdecl lldiv(  long long _Numerator,   long long _Denominator);

// These functions have declspecs in their declarations in the Windows headers,
// which cause PREfast to fire 6540.
#pragma warning(push)
#pragma warning(disable: 6540)

unsigned int __cdecl _rotl(
      unsigned int _Value,
      int          _Shift
    );

 
unsigned long __cdecl _lrotl(
      unsigned long _Value,
      int           _Shift
    );

unsigned __int64 __cdecl _rotl64(
      unsigned __int64 _Value,
      int              _Shift
    );

unsigned int __cdecl _rotr(
      unsigned int _Value,
      int          _Shift
    );

 
unsigned long __cdecl _lrotr(
      unsigned long _Value,
      int           _Shift
    );

unsigned __int64 __cdecl _rotr64(
      unsigned __int64 _Value,
      int              _Shift
    );

#pragma warning(pop)



// Maximum value that can be returned by the rand function:


 void __cdecl srand(  unsigned int _Seed);

   int __cdecl rand(void);



#line 357 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"































// Structs used to fool the compiler into not generating floating point
// instructions when copying and pushing [long] double values




    #pragma pack(push, 4)
    typedef struct
    {
        unsigned char ld[10];
    } _LDOUBLE;
    #pragma pack(pop)

    











#line 414 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

typedef struct
{
    double x;
} _CRT_DOUBLE;

typedef struct
{
    float f;
} _CRT_FLOAT;

// push and pop long, which is #defined as __int64 by a spec2k test



typedef struct
{
    long double x;
} _LONGDOUBLE;



#pragma pack(push, 4)
typedef struct
{
    unsigned char ld12[12];
} _LDBL12;
#pragma pack(pop)



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Narrow String to Number Conversions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                      double    __cdecl atof   (  char const* _String);
    int       __cdecl atoi   (  char const* _String);
                      long      __cdecl atol   (  char const* _String);
                      long long __cdecl atoll  (  char const* _String);
                      __int64   __cdecl _atoi64(  char const* _String);

   double    __cdecl _atof_l  (  char const* _String,   _locale_t _Locale);
   int       __cdecl _atoi_l  (  char const* _String,   _locale_t _Locale);
   long      __cdecl _atol_l  (  char const* _String,   _locale_t _Locale);
   long long __cdecl _atoll_l (  char const* _String,   _locale_t _Locale);
   __int64   __cdecl _atoi64_l(  char const* _String,   _locale_t _Locale);

   int __cdecl _atoflt (  _CRT_FLOAT*  _Result,   char const* _String);
   int __cdecl _atodbl (  _CRT_DOUBLE* _Result,   char*       _String);
   int __cdecl _atoldbl(  _LDOUBLE*    _Result,   char*       _String);

 
 int __cdecl _atoflt_l(
         _CRT_FLOAT* _Result,
        char const* _String,
      _locale_t   _Locale
    );

 
 int __cdecl _atodbl_l(
         _CRT_DOUBLE* _Result,
        char*        _String,
      _locale_t    _Locale
    );


 
 int __cdecl _atoldbl_l(
         _LDOUBLE* _Result,
        char*     _String,
      _locale_t _Locale
    );

 
 float __cdecl strtof(
                        char const* _String,
        char**      _EndPtr
    );

 
 float __cdecl _strtof_l(
                        char const* _String,
        char**      _EndPtr,
                      _locale_t   _Locale
    );

 
 double __cdecl strtod(
                        char const* _String,
        char**      _EndPtr
    );

 
 double __cdecl _strtod_l(
                        char const* _String,
        char**      _EndPtr,
                      _locale_t   _Locale
    );

 
 long double __cdecl strtold(
                        char const* _String,
        char**      _EndPtr
    );

 
 long double __cdecl _strtold_l(
                        char const* _String,
        char**      _EndPtr,
                      _locale_t   _Locale
    );

 
 long __cdecl strtol(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix
    );

 
 long __cdecl _strtol_l(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix,
                      _locale_t   _Locale
    );

 
 long long __cdecl strtoll(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix
    );

 
 long long __cdecl _strtoll_l(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix,
                      _locale_t   _Locale
    );

 
 unsigned long __cdecl strtoul(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix
    );

 
 unsigned long __cdecl _strtoul_l(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix,
                      _locale_t   _Locale
    );

 
 unsigned long long __cdecl strtoull(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix
    );

 
 unsigned long long __cdecl _strtoull_l(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix,
                      _locale_t   _Locale
    );

 
 __int64 __cdecl _strtoi64(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix
    );

 
 __int64 __cdecl _strtoi64_l(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix,
                      _locale_t   _Locale
    );

 
 unsigned __int64 __cdecl _strtoui64(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix
    );

 
 unsigned __int64 __cdecl _strtoui64_l(
                        char const* _String,
        char**      _EndPtr,
                          int         _Radix,
                      _locale_t   _Locale
    );



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Number to Narrow String Conversions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 

 errno_t __cdecl _itoa_s(
                              int    _Value,
      char*  _Buffer,
                              size_t _BufferCount,
                              int    _Radix
    );


#line 640 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_itoa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _itoa( int _Value,   char *_Buffer,  int _Radix);
#line 647 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

 

 errno_t __cdecl _ltoa_s(
                              long   _Value,
      char*  _Buffer,
                              size_t _BufferCount,
                              int    _Radix
    );


#line 663 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_ltoa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _ltoa( long _Value,   char *_Buffer,  int _Radix);
#line 670 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

 

 errno_t __cdecl _ultoa_s(
                              unsigned long _Value,
      char*         _Buffer,
                              size_t        _BufferCount,
                              int           _Radix
    );


#line 686 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_ultoa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _ultoa( unsigned long _Value,   char *_Buffer,  int _Radix);
#line 693 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

 

 errno_t __cdecl _i64toa_s(
                              __int64 _Value,
      char*   _Buffer,
                              size_t  _BufferCount,
                              int     _Radix
    );

 
__declspec(deprecated("This function or variable may be unsafe. Consider using " "_i64toa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _i64toa(
                        __int64 _Value,
        char*   _Buffer,
                        int     _Radix
    );

 

 errno_t __cdecl _ui64toa_s(
                              unsigned __int64 _Value,
      char*            _Buffer,
                              size_t           _BufferCount,
                              int              _Radix
    );

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_ui64toa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _ui64toa(
                        unsigned __int64 _Value,
        char*            _Buffer,
                        int              _Radix
    );



// _CVTBUFSIZE is the maximum size for the per-thread conversion buffer.  It
// should be at least as long as the number of digits in the largest double
// precision value (?.?e308 in IEEE arithmetic).  We will use the same size
// buffer as is used in the printf support routines.
//
// (This value actually allows 40 additional decimal places; even though there
// are only 16 digits of accuracy in a double precision IEEE number, the user may
// ask for more to effect zero padding.)


 

 errno_t __cdecl _ecvt_s(
      char* _Buffer,
       size_t                       _BufferCount,
       double                       _Value,
       int                          _DigitCount,
      int*                         _PtDec,
      int*                         _PtSign
    );


#line 758 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ecvt_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _ecvt(
       double _Value,
       int    _DigitCount,
      int*   _PtDec,
      int*   _PtSign
    );

 

 errno_t __cdecl _fcvt_s(
      char*  _Buffer,
                              size_t _BufferCount,
                              double _Value,
                              int    _FractionalDigitCount,
                             int*   _PtDec,
                             int*   _PtSign
    );


#line 787 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_fcvt_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _fcvt(
       double _Value,
       int    _FractionalDigitCount,
      int*   _PtDec,
      int*   _PtSign
    );

 
 errno_t __cdecl _gcvt_s(
      char*  _Buffer,
                              size_t _BufferCount,
                              double _Value,
                              int    _DigitCount
    );


#line 812 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_gcvt_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _gcvt(
                        double _Value,
                        int    _DigitCount,
        char*  _Buffer
    );



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Multibyte String Operations and Conversions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// Maximum number of bytes in multi-byte character in the current locale
// (also defined in ctype.h).

    

#line 833 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
        
    #line 835 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

    


        
    #line 841 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

     
     int __cdecl ___mb_cur_max_func(void);

     
     int __cdecl ___mb_cur_max_l_func(_locale_t _Locale);
#line 848 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



 
 int __cdecl mblen(
        char const* _Ch,
                                             size_t      _MaxCount
    );

 
 int __cdecl _mblen_l(
        char const* _Ch,
                                             size_t      _MaxCount,
                                         _locale_t   _Locale
    );

 
 
 size_t __cdecl _mbstrlen(
      char const* _String
    );

 
 
 size_t __cdecl _mbstrlen_l(
        char const* _String,
      _locale_t   _Locale
    );

 
 
 size_t __cdecl _mbstrnlen(
      char const* _String,
        size_t      _MaxCount
    );

 
 
 size_t __cdecl _mbstrnlen_l(
        char const* _String,
          size_t      _MaxCount,
      _locale_t   _Locale
    );

 
 int __cdecl mbtowc(
                      wchar_t*    _DstCh,
      char const* _SrcCh,
                                      size_t      _SrcSizeInBytes
    );

 
 int __cdecl _mbtowc_l(
                      wchar_t*    _DstCh,
      char const* _SrcCh,
                                      size_t      _SrcSizeInBytes,
                                  _locale_t   _Locale
    );


 errno_t __cdecl mbstowcs_s(
                                                      size_t*     _PtNumOfCharConverted,
      wchar_t*    _DstBuf,
                                                           size_t      _SizeInWords,
                                     char const* _SrcBuf,
                                                           size_t      _MaxCount
    );


#line 923 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "mbstowcs_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  size_t __cdecl mbstowcs( wchar_t *_Dest,  char const* _Source,  size_t _MaxCount);
#line 930 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"


 errno_t __cdecl _mbstowcs_s_l(
                                                      size_t*     _PtNumOfCharConverted,
      wchar_t*    _DstBuf,
                                                           size_t      _SizeInWords,
                                     char const* _SrcBuf,
                                                           size_t      _MaxCount,
                                                       _locale_t   _Locale
    );


#line 949 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_mbstowcs_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  size_t __cdecl _mbstowcs_l(  wchar_t *_Dest,   char const* _Source,   size_t _MaxCount,   _locale_t _Locale);
#line 958 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"




__declspec(deprecated("This function or variable may be unsafe. Consider using " "wctomb_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 int __cdecl wctomb(
      char*   _MbCh,
                                wchar_t _WCh
    );

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wctomb_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 int __cdecl _wctomb_l(
        char*     _MbCh,
                          wchar_t   _WCh,
                      _locale_t _Locale
    );



    
     errno_t __cdecl wctomb_s(
                                                         int*    _SizeConverted,
          char*   _MbCh,
                                                              rsize_t _SizeInBytes,
                                                              wchar_t _WCh
        );

#line 986 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"


 errno_t __cdecl _wctomb_s_l(
                             int*     _SizeConverted,
      char*     _MbCh,
                                  size_t    _SizeInBytes,
                                  wchar_t   _WCh,
                              _locale_t _Locale);


 errno_t __cdecl wcstombs_s(
                                                               size_t*        _PtNumOfCharConverted,
      char*          _Dst,
                                                                    size_t         _DstSizeInBytes,
                                                                  wchar_t const* _Src,
                                                                    size_t         _MaxCountInBytes
    );


#line 1011 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "wcstombs_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  size_t __cdecl wcstombs( char *_Dest,  wchar_t const* _Source,  size_t _MaxCount);
#line 1018 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"


 errno_t __cdecl _wcstombs_s_l(
                                                               size_t*        _PtNumOfCharConverted,
      char*          _Dst,
                                                                    size_t         _DstSizeInBytes,
                                                                  wchar_t const* _Src,
                                                                    size_t         _MaxCountInBytes,
                                                                _locale_t      _Locale
    );


#line 1037 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcstombs_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  size_t __cdecl _wcstombs_l(  char *_Dest,   wchar_t const* _Source,   size_t _MaxCount,   _locale_t _Locale);
#line 1046 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Path Manipulation
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// Sizes for buffers used by the _makepath() and _splitpath() functions.
// note that the sizes include space for 0-terminator










 
 
 __declspec(allocator) char* __cdecl _fullpath(
      char*       _Buffer,
                                char const* _Path,
                                  size_t      _BufferCount
    );




 errno_t __cdecl _makepath_s(
      char*       _Buffer,
                              size_t      _BufferCount,
                        char const* _Drive,
                        char const* _Dir,
                        char const* _Filename,
                        char const* _Ext
    );


#line 1094 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_makepath_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  void __cdecl _makepath(  char *_Buffer,  char const* _Drive,  char const* _Dir,  char const* _Filename,  char const* _Ext);
#line 1103 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_splitpath_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 void __cdecl _splitpath(
                        char const* _FullPath,
        char*       _Drive,
        char*       _Dir,
        char*       _Filename,
        char*       _Ext
    );


 errno_t __cdecl _splitpath_s(
                                  char const* _FullPath,
         char*       _Drive,
                                    size_t      _DriveCount,
           char*       _Dir,
                                    size_t      _DirCount,
      char*       _Filename,
                                    size_t      _FilenameCount,
           char*       _Ext,
                                    size_t      _ExtCount
    );






 
 errno_t __cdecl getenv_s(
                                 size_t*     _RequiredCount,
      char*       _Buffer,
                                  rsize_t     _BufferCount,
                                char const* _VarName
    );

#line 1140 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"




 int*       __cdecl __p___argc (void);
 char***    __cdecl __p___argv (void);
 wchar_t*** __cdecl __p___wargv(void);






    
    
    
#line 1157 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

 char***    __cdecl __p__environ (void);
 wchar_t*** __cdecl __p__wenviron(void);


    
#line 1164 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"







    
    
#line 1174 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



// Sizes for buffers used by the getenv/putenv family of functions.





      __declspec(deprecated("This function or variable may be unsafe. Consider using " "_dupenv_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl getenv(
          char const* _VarName
        );

    
#line 1194 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

    


#line 1199 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

    
     errno_t __cdecl _dupenv_s(
            char**      _Buffer,
                                                                            size_t*     _BufferCount,
                                                                               char const* _VarName
        );

    

#line 1210 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

     int __cdecl system(
          char const* _Command
        );

    // The functions below have declspecs in their declarations in the Windows
    // headers, causing PREfast to fire 6540 here
    #pragma warning(push)
    #pragma warning(disable: 6540)

     
     int __cdecl _putenv(
          char const* _EnvString
        );

    
     errno_t __cdecl _putenv_s(
          char const* _Name,
          char const* _Value
        );

    #pragma warning(pop)

     errno_t __cdecl _searchenv_s(
                                char const* _Filename,
                                char const* _VarName,
          char*       _Buffer,
                                  size_t      _BufferCount
        );

    
#line 1246 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_searchenv_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  void __cdecl _searchenv( char const* _Filename,  char const* _VarName,   char *_Buffer);
#line 1253 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

    // The Win32 API SetErrorMode, Beep and Sleep should be used instead.
    __declspec(deprecated("This function or variable has been superceded by newer library " "or operating system functionality. Consider using " "SetErrorMode" " " "instead. See online help for details."))
     void __cdecl _seterrormode(
          int _Mode
        );

    __declspec(deprecated("This function or variable has been superceded by newer library " "or operating system functionality. Consider using " "Beep" " " "instead. See online help for details."))
     void __cdecl _beep(
          unsigned _Frequency,
          unsigned _Duration
        );

    __declspec(deprecated("This function or variable has been superceded by newer library " "or operating system functionality. Consider using " "Sleep" " " "instead. See online help for details."))
     void __cdecl _sleep(
          unsigned long _Duration
        );

#line 1272 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"


//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Non-ANSI Names for Compatibility
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


    
        
        
    #line 1285 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"

    
    

    #pragma warning(push)
    #pragma warning(disable: 4141) 

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_ecvt" ". See online help for details.")) __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ecvt_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl ecvt(
           double _Value,
           int    _DigitCount,
          int*   _PtDec,
          int*   _PtSign
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_fcvt" ". See online help for details.")) __declspec(deprecated("This function or variable may be unsafe. Consider using " "_fcvt_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl fcvt(
           double _Value,
           int    _FractionalDigitCount,
          int*   _PtDec,
          int*   _PtSign
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_gcvt" ". See online help for details.")) __declspec(deprecated("This function or variable may be unsafe. Consider using " "_fcvt_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl gcvt(
                            double _Value,
                            int    _DigitCount,
            char*  _DstBuf
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_itoa" ". See online help for details.")) __declspec(deprecated("This function or variable may be unsafe. Consider using " "_itoa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl itoa(
                            int   _Value,
            char* _Buffer,
                            int   _Radix
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_ltoa" ". See online help for details.")) __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ltoa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl ltoa(
                            long  _Value,
            char* _Buffer,
                            int   _Radix
        );


    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_swab" ". See online help for details."))
     void __cdecl swab(
          char* _Buf1,
          char* _Buf2,
                                     int   _SizeInBytes
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_ultoa" ". See online help for details.")) __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ultoa_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     char* __cdecl ultoa(
                            unsigned long _Value,
            char*         _Buffer,
                            int           _Radix
        );

    

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_putenv" ". See online help for details."))
     int __cdecl putenv(
          char const* _EnvString
        );

    #pragma warning(pop)

    _onexit_t __cdecl onexit(  _onexit_t _Func);

#line 1356 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 1363 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdlib.h"
#line 17 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
//
// string.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <string.h> header.
//
#pragma once




#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"
//
// corecrt_memory.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The buffer (memory) manipulation library.  These declarations are split out
// so that they may be included by both <string.h> and <memory.h>.  <string.h>
// does not include <memory.h> to avoid introducing conflicts with other user
// headers named <memory.h>.
//
#pragma once


#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memcpy_s.h"
//
// corecrt_memcpy_s.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Inline definitions of memcpy_s and memmove_s
//
#pragma once


#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\errno.h"
//
// errno.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// System error numbers for use with errno and errno_t.
//
#pragma once





#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))




     int* __cdecl _errno(void);
    

     errno_t __cdecl _set_errno(  int _Value);
     errno_t __cdecl _get_errno(  int* _Value);

     unsigned long* __cdecl __doserrno(void);
    

     errno_t __cdecl _set_doserrno(  unsigned long _Value);
     errno_t __cdecl _get_doserrno(  unsigned long * _Value);
#line 35 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\errno.h"



// Error codes




































// Error codes used in the Secure CRT functions

    
    
    
    
    
#line 83 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\errno.h"

// Support EDEADLOCK for compatibility with older Microsoft C versions


// POSIX Supplement

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
#line 131 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\errno.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 138 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\errno.h"
#line 12 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memcpy_s.h"
#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime_string.h"
//
// vcruntime_string.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// <string.h> functionality that is implemented in the VCRuntime.
//
#pragma once



#pragma warning(push)
#pragma warning(disable:   4514 4820 )



__pragma(pack(push, 8))



  
 void * __cdecl memchr(
      void const* _Buf,
                                 int         _Val,
                                 size_t      _MaxCount
    );

  
int __cdecl memcmp(
      void const* _Buf1,
      void const* _Buf2,
                         size_t      _Size
    );


 

#line 43 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime_string.h"
void* __cdecl memcpy(
      void* _Dst,
            void const* _Src,
                               size_t      _Size
    );


 void* __cdecl memmove(
      void*       _Dst,
            void const* _Src,
                                   size_t      _Size
    );

 

#line 63 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime_string.h"
void* __cdecl memset(
      void*  _Dst,
                               int    _Val,
                               size_t _Size
    );

  
 char * __cdecl strchr(
      char const* _Str,
        int         _Val
    );

  
 char * __cdecl strrchr(
      char const* _Str,
        int         _Ch
    );

    
 char * __cdecl strstr(
      char const* _Str,
      char const* _SubStr
    );

  

 wchar_t * __cdecl wcschr(
      wchar_t const* _Str,
        wchar_t        _Ch
    );

  
 wchar_t * __cdecl wcsrchr(
      wchar_t const* _Str,
        wchar_t        _Ch
    );

    

 wchar_t * __cdecl wcsstr(
      wchar_t const* _Str,
      wchar_t const* _SubStr
    );



__pragma(pack(pop))

#line 112 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\vcruntime_string.h"

#pragma warning(pop) 
#line 13 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memcpy_s.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))


    
#line 23 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memcpy_s.h"














     
    
    static __inline errno_t __cdecl memcpy_s(
          void*       const _Destination,
                                                              rsize_t     const _DestinationSize,
                                 void const* const _Source,
                                                              rsize_t     const _SourceSize
        )
    {
        if (_SourceSize == 0)
        {
            return 0;
        }

        { int _Expr_val=!!(_Destination != ((void *)0)); if (!(_Expr_val)) { (*_errno()) = 22; _invalid_parameter_noinfo(); return 22; } };
        if (_Source == ((void *)0) || _DestinationSize < _SourceSize)
        {
            memset(_Destination, 0, _DestinationSize);

            { int _Expr_val=!!(_Source != ((void *)0)); if (!(_Expr_val)) { (*_errno()) = 22; _invalid_parameter_noinfo(); return 22; } };
            { int _Expr_val=!!(_DestinationSize >= _SourceSize); if (!(_Expr_val)) { (*_errno()) = 34; _invalid_parameter_noinfo(); return 34; } };

            // Unreachable, but required to suppress /analyze warnings:
            return 22;
        }
        memcpy(_Destination, _Source, _SourceSize);
        return 0;
    }

    
    static __inline errno_t __cdecl memmove_s(
          void*       const _Destination,
                                                              rsize_t     const _DestinationSize,
                                 void const* const _Source,
                                                              rsize_t     const _SourceSize
        )
    {
        if (_SourceSize == 0)
        {
            return 0;
        }

        { int _Expr_val=!!(_Destination != ((void *)0)); if (!(_Expr_val)) { (*_errno()) = 22; _invalid_parameter_noinfo(); return 22; } };
        { int _Expr_val=!!(_Source != ((void *)0)); if (!(_Expr_val)) { (*_errno()) = 22; _invalid_parameter_noinfo(); return 22; } };
        { int _Expr_val=!!(_DestinationSize >= _SourceSize); if (!(_Expr_val)) { (*_errno()) = 34; _invalid_parameter_noinfo(); return 34; } };

        memmove(_Destination, _Source, _SourceSize);
        return 0;
    }

#line 88 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memcpy_s.h"




#pragma warning(pop) 
__pragma(pack(pop))
#line 15 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"


#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )




__pragma(pack(push, 8))



 
 int __cdecl _memicmp(
      void const* _Buf1,
      void const* _Buf2,
                             size_t      _Size
    );

 
 int __cdecl _memicmp_l(
      void const* _Buf1,
      void const* _Buf2,
                             size_t      _Size,
                         _locale_t   _Locale
    );





    












#line 60 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"

    












#line 75 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"

#line 77 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"





    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_memccpy" ". See online help for details."))
     void* __cdecl memccpy(
          void*       _Dst,
            void const* _Src,
                                   int         _Val,
                                   size_t      _Size
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_memicmp" ". See online help for details."))
     int __cdecl memicmp(
          void const* _Buf1,
          void const* _Buf2,
                                 size_t      _Size
        );

#line 98 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"
















#line 115 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"



__pragma(pack(pop))

#line 121 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_memory.h"

#pragma warning(pop) 
#line 14 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"
//
// corecrt_wstring.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// This file declares the wide character (wchar_t) string functionality, shared
// by <string.h> and <wchar.h>.
//
#pragma once




#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )




__pragma(pack(push, 8))



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Secure Alternatives
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


    
     errno_t __cdecl wcscat_s(
          wchar_t* _Destination,
          rsize_t _SizeInWords,
          wchar_t const* _Source
        );

    
     errno_t __cdecl wcscpy_s(
          wchar_t* _Destination,
          rsize_t _SizeInWords,
          wchar_t const* _Source
        );

    
     errno_t __cdecl wcsncat_s(
          wchar_t*       _Destination,
                                     rsize_t        _SizeInWords,
               wchar_t const* _Source,
                                     rsize_t        _MaxCount
        );

    
     errno_t __cdecl wcsncpy_s(
          wchar_t*       _Destination,
                                  rsize_t        _SizeInWords,
            wchar_t const* _Source,
                                  rsize_t        _MaxCount
        );

     
     wchar_t* __cdecl wcstok_s(
                          wchar_t*       _String,
                                 wchar_t const* _Delimiter,
            wchar_t**      _Context
        );

#line 69 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Wide-Character <string.h> Functions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+



#line 81 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 
 __declspec(allocator) wchar_t* __cdecl _wcsdup(
      wchar_t const* _String
    );



#line 90 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"




#line 98 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"


    __declspec(deprecated("This function or variable may be unsafe. Consider using " "wcscat_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl wcscat( wchar_t *_Destination,  wchar_t const* _Source);
#line 105 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"
#line 106 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 
 int __cdecl wcscmp(
      wchar_t const* _String1,
      wchar_t const* _String2
    );


#line 118 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "wcscpy_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl wcscpy( wchar_t *_Destination,  wchar_t const* _Source);
#line 124 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 
 size_t __cdecl wcscspn(
      wchar_t const* _String,
      wchar_t const* _Control
    );

 
 size_t __cdecl wcslen(
      wchar_t const* _String
    );

 

#line 141 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

#line 145 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"
 size_t __cdecl wcsnlen(
      wchar_t const* _Source,
                            size_t         _MaxCount
    );



     
    
#line 157 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"
    
#line 161 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"
    static __inline size_t __cdecl wcsnlen_s(
          wchar_t const* _Source,
                                size_t         _MaxCount
        )
    {
        return (_Source == 0) ? 0 : wcsnlen(_Source, _MaxCount);
    }

#line 170 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"


#line 177 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "wcsncat_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl wcsncat(  wchar_t *_Destination,   wchar_t const* _Source,   size_t _Count);
#line 185 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 
 int __cdecl wcsncmp(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount
    );


#line 199 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "wcsncpy_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl wcsncpy(    wchar_t *_Destination,   wchar_t const* _Source,   size_t _Count);
#line 207 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 
 wchar_t * __cdecl wcspbrk(
      wchar_t const* _String,
      wchar_t const* _Control
    );

 
 size_t __cdecl wcsspn(
      wchar_t const* _String,
      wchar_t const* _Control
    );

  __declspec(deprecated("This function or variable may be unsafe. Consider using " "wcstok_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 wchar_t* __cdecl wcstok(
                          wchar_t*       _String,
                                 wchar_t const* _Delimiter,
        wchar_t**      _Context
    );



    

#line 232 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"
        



    #line 237 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "wcstok_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    static __inline wchar_t* __cdecl _wcstok(
          wchar_t*       const _String,
                 wchar_t const* const _Delimiter
        )
    {
        return wcstok(_String, _Delimiter, 0);
    }

    

#line 250 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

    








#line 261 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

#line 263 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"



 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcserror_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 wchar_t* __cdecl _wcserror(
      int _ErrorNumber
    );


 errno_t __cdecl _wcserror_s(
      wchar_t* _Buffer,
                                  size_t   _SizeInWords,
                                  int      _ErrorNumber
    );


#line 284 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 
 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "__wcserror_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 wchar_t* __cdecl __wcserror(
      wchar_t const* _String
    );

  errno_t __cdecl __wcserror_s(
      wchar_t*       _Buffer,
                                  size_t         _SizeInWords,
                                wchar_t const* _ErrorMessage
    );


#line 303 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

   int __cdecl _wcsicmp(
      wchar_t const* _String1,
      wchar_t const* _String2
    );

   int __cdecl _wcsicmp_l(
        wchar_t const* _String1,
        wchar_t const* _String2,
      _locale_t      _Locale
    );

   int __cdecl _wcsnicmp(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount
    );

   int __cdecl _wcsnicmp_l(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount,
                        _locale_t      _Locale
    );

  errno_t __cdecl _wcsnset_s(
      wchar_t* _Destination,
                                 size_t   _SizeInWords,
                                 wchar_t  _Value,
                                 size_t   _MaxCount
    );


#line 341 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcsnset_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _wcsnset(  wchar_t *_String,   wchar_t _Value,   size_t _MaxCount);
#line 349 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 wchar_t* __cdecl _wcsrev(
      wchar_t* _String
    );

  errno_t __cdecl _wcsset_s(
      wchar_t* _Destination,
                                 size_t   _SizeInWords,
                                 wchar_t  _Value
    );


#line 365 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcsset_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _wcsset(  wchar_t *_String,   wchar_t _Value);
#line 372 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

  errno_t __cdecl _wcslwr_s(
      wchar_t* _String,
                                 size_t   _SizeInWords
    );


#line 382 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcslwr_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _wcslwr( wchar_t *_String);
#line 387 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"


 errno_t __cdecl _wcslwr_s_l(
      wchar_t*  _String,
                                 size_t    _SizeInWords,
                             _locale_t _Locale
    );


#line 400 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcslwr_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _wcslwr_l(  wchar_t *_String,   _locale_t _Locale);
#line 407 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"


 errno_t __cdecl _wcsupr_s(
      wchar_t* _String,
                          size_t   _Size
    );


#line 418 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcsupr_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _wcsupr( wchar_t *_String);
#line 423 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"


 errno_t __cdecl _wcsupr_s_l(
      wchar_t*  _String,
                          size_t    _Size,
                      _locale_t _Locale
    );


#line 436 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wcsupr_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  wchar_t* __cdecl _wcsupr_l(  wchar_t *_String,   _locale_t _Locale);
#line 443 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

 

 size_t __cdecl wcsxfrm(
        wchar_t*       _Destination,
                                         wchar_t const* _Source,
                size_t         _MaxCount
    );

 

 size_t __cdecl _wcsxfrm_l(
        wchar_t*       _Destination,
                                         wchar_t const* _Source,
                size_t         _MaxCount,
                                       _locale_t      _Locale
    );

 
 int __cdecl wcscoll(
      wchar_t const* _String1,
      wchar_t const* _String2
    );

 
 int __cdecl _wcscoll_l(
        wchar_t const* _String1,
        wchar_t const* _String2,
      _locale_t      _Locale
    );

 
 int __cdecl _wcsicoll(
      wchar_t const* _String1,
      wchar_t const* _String2
    );

 
 int __cdecl _wcsicoll_l(
        wchar_t const* _String1,
        wchar_t const* _String2,
      _locale_t      _Locale
    );

 
 int __cdecl _wcsncoll(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount
    );

 
 int __cdecl _wcsncoll_l(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount,
                        _locale_t      _Locale
    );

 
 int __cdecl _wcsnicoll(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount
    );

 
 int __cdecl _wcsnicoll_l(
      wchar_t const* _String1,
      wchar_t const* _String2,
                            size_t         _MaxCount,
                        _locale_t      _Locale
    );



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Inline C++ Overloads
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


































//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Non-Standard Names
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    


#line 568 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsdup" ". See online help for details."))
     wchar_t* __cdecl wcsdup(
          wchar_t const* _String
        );

    

#line 577 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

    // Declarations of functions defined in oldnames.lib:
    

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsicmp" ". See online help for details."))
     int __cdecl wcsicmp(
          wchar_t const* _String1,
          wchar_t const* _String2
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsnicmp" ". See online help for details."))
     int __cdecl wcsnicmp(
          wchar_t const* _String1,
          wchar_t const* _String2,
                                size_t         _MaxCount
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsnset" ". See online help for details."))
     
     wchar_t* __cdecl wcsnset(
          wchar_t* _String,
                                  wchar_t  _Value,
                                  size_t   _MaxCount
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsrev" ". See online help for details."))
     
     wchar_t* __cdecl wcsrev(
          wchar_t* _String
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsset" ". See online help for details."))
     
     wchar_t* __cdecl wcsset(
          wchar_t* _String,
               wchar_t  _Value
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcslwr" ". See online help for details."))
     
     wchar_t* __cdecl wcslwr(
          wchar_t* _String
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsupr" ". See online help for details."))
     
     wchar_t* __cdecl wcsupr(
          wchar_t* _String
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_wcsicoll" ". See online help for details."))
     int __cdecl wcsicoll(
          wchar_t const* _String1,
          wchar_t const* _String2
        );

#line 634 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"



__pragma(pack(pop))

#line 640 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstring.h"

#pragma warning(pop) 
#line 15 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"




#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))







    
     errno_t __cdecl strcpy_s(
          char*       _Destination,
                                  rsize_t     _SizeInBytes,
                                char const* _Source
        );

    
     errno_t __cdecl strcat_s(
          char*       _Destination,
                                     rsize_t     _SizeInBytes,
                                   char const* _Source
        );

    
     errno_t __cdecl strerror_s(
          char*  _Buffer,
                                  size_t _SizeInBytes,
                                  int    _ErrorNumber);

    
     errno_t __cdecl strncat_s(
          char*       _Destination,
                                     rsize_t     _SizeInBytes,
               char const* _Source,
                                     rsize_t     _MaxCount
        );

    
     errno_t __cdecl strncpy_s(
          char*       _Destination,
                                  rsize_t     _SizeInBytes,
            char const* _Source,
                                  rsize_t     _MaxCount
        );

     
     char*  __cdecl strtok_s(
                          char*       _String,
                                 char const* _Delimiter,
            char**      _Context
        );

#line 75 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 void* __cdecl _memccpy(
      void*       _Dst,
                                   void const* _Src,
                                   int         _Val,
                                   size_t      _MaxCount
    );


#line 88 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"



    __declspec(deprecated("This function or variable may be unsafe. Consider using " "strcat_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl strcat( char *_Destination,  char const* _Source);
#line 96 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

#line 98 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
int __cdecl strcmp(
      char const* _Str1,
      char const* _Str2
    );

 
 int __cdecl _strcmpi(
      char const* _String1,
      char const* _String2
    );

 
 int __cdecl strcoll(
      char const* _String1,
      char const* _String2
    );

 
 int __cdecl _strcoll_l(
        char const* _String1,
        char const* _String2,
      _locale_t   _Locale
    );


#line 129 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "strcpy_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl strcpy( char *_Destination,  char const* _Source);
#line 135 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 size_t __cdecl strcspn(
      char const* _Str,
      char const* _Control
    );




#line 146 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 __declspec(allocator) char* __cdecl _strdup(
      char const* _Source
    );



#line 155 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_strerror_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char*  __cdecl _strerror(
      char const* _ErrorMessage
    );


 errno_t __cdecl _strerror_s(
      char*       _Buffer,
                              size_t      _SizeInBytes,
                        char const* _ErrorMessage
    );


#line 175 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "strerror_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl strerror(
      int _ErrorMessage
    );


#line 187 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 int __cdecl _stricmp(
      char const* _String1,
      char const* _String2
    );

 
 int __cdecl _stricoll(
      char const* _String1,
      char const* _String2
    );

 
 int __cdecl _stricoll_l(
        char const* _String1,
        char const* _String2,
      _locale_t   _Locale
    );

 
 int __cdecl _stricmp_l(
        char const* _String1,
        char const* _String2,
      _locale_t   _Locale
    );

 
size_t __cdecl strlen(
      char const* _Str
    );


 errno_t __cdecl _strlwr_s(
      char*  _String,
                          size_t _Size
    );


#line 229 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strlwr_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strlwr( char *_String);
#line 234 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"


 errno_t __cdecl _strlwr_s_l(
      char*     _String,
                          size_t    _Size,
                      _locale_t _Locale
    );


#line 247 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strlwr_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strlwr_l(  char *_String,   _locale_t _Locale);
#line 254 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"


#line 261 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "strncat_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl strncat(  char *_Destination,   char const* _Source,   size_t _Count);
#line 269 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 int __cdecl strncmp(
      char const* _Str1,
      char const* _Str2,
                            size_t      _MaxCount
    );

 
 int __cdecl _strnicmp(
      char const* _String1,
      char const* _String2,
                            size_t      _MaxCount
    );

 
 int __cdecl _strnicmp_l(
      char const* _String1,
      char const* _String2,
                            size_t      _MaxCount,
                        _locale_t   _Locale
    );

 
 int __cdecl _strnicoll(
      char const* _String1,
      char const* _String2,
                            size_t      _MaxCount
    );

 
 int __cdecl _strnicoll_l(
      char const* _String1,
      char const* _String2,
                            size_t      _MaxCount,
                        _locale_t   _Locale
    );

 
 int __cdecl _strncoll(
      char const* _String1,
      char const* _String2,
                            size_t      _MaxCount
    );

 
 int __cdecl _strncoll_l(
      char const* _String1,
      char const* _String2,
                            size_t      _MaxCount,
                        _locale_t   _Locale
    );

 size_t __cdecl __strncnt(
      char const* _String,
                         size_t      _Count
    );


#line 333 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "strncpy_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl strncpy(    char *_Destination,   char const* _Source,   size_t _Count);
#line 341 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 

#line 347 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

#line 351 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
 size_t __cdecl strnlen(
      char const* _String,
                            size_t      _MaxCount
    );



     
    
#line 363 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
    
#line 367 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
    static __inline size_t __cdecl strnlen_s(
          char const* _String,
                                size_t      _MaxCount
        )
    {
        return _String == 0 ? 0 : strnlen(_String, _MaxCount);
    }

#line 376 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"


 errno_t __cdecl _strnset_s(
      char*  _String,
                                 size_t _SizeInBytes,
                                 int    _Value,
                                 size_t _MaxCount
    );


#line 391 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strnset_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strnset(  char *_Destination,   int _Value,   size_t _Count);
#line 399 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 char * __cdecl strpbrk(
      char const* _Str,
      char const* _Control
    );

 char* __cdecl _strrev(
      char* _Str
    );


 errno_t __cdecl _strset_s(
      char*  _Destination,
                                     size_t _DestinationSize,
                                     int    _Value
    );


#line 422 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strset_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strset( char *_Destination,  int _Value);
#line 428 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 
 size_t __cdecl strspn(
      char const* _Str,
      char const* _Control
    );

  __declspec(deprecated("This function or variable may be unsafe. Consider using " "strtok_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl strtok(
      char*       _String,
             char const* _Delimiter
    );


 errno_t __cdecl _strupr_s(
      char*  _String,
                          size_t _Size
    );


#line 451 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strupr_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strupr( char *_String);
#line 456 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"


 errno_t __cdecl _strupr_s_l(
      char*     _String,
                          size_t    _Size,
                      _locale_t _Locale
    );


#line 469 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strupr_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strupr_l(  char *_String,   _locale_t _Locale);
#line 476 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"

 

 size_t __cdecl strxfrm(
        char*       _Destination,
                                         char const* _Source,
                 size_t      _MaxCount
    );

 

 size_t __cdecl _strxfrm_l(
        char*       _Destination,
                                         char const* _Source,
                 size_t      _MaxCount,
                                       _locale_t   _Locale
    );




































    
    
      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strdup" ". See online help for details."))
     char* __cdecl strdup(
          char const* _String
        );
    

    // Declarations of functions defined in oldnames.lib:
      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strcmpi" ". See online help for details."))
     int __cdecl strcmpi(
          char const* _String1,
          char const* _String2
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_stricmp" ". See online help for details."))
     int __cdecl stricmp(
          char const* _String1,
          char const* _String2
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strlwr" ". See online help for details."))
     char* __cdecl strlwr(
          char* _String
        );

      __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strnicmp" ". See online help for details."))
     int __cdecl strnicmp(
          char const* _String1,
          char const* _String2,
                                size_t      _MaxCount
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strnset" ". See online help for details."))
     char* __cdecl strnset(
          char*  _String,
                                  int    _Value,
                                  size_t _MaxCount
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strrev" ". See online help for details."))
     char* __cdecl strrev(
          char* _String
        );

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strset" ". See online help for details."))
    char* __cdecl strset(
          char* _String,
               int   _Value);

    __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_strupr" ". See online help for details."))
     char* __cdecl strupr(
          char* _String
        );

#line 585 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 592 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
#line 593 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\string.h"
#line 18 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
//
// stdio.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <stdio.h> header.
//
#pragma once




#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
//
// corecrt_wstdio.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// This file declares the wide character (wchar_t) I/O functionality, shared by
// <stdio.h> and <wchar.h>.  It also defines several core I/O types, which are
// also shared by those two headers.
//
#pragma once


#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"
//
// corecrt_stdio_config.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Per-module <stdio.h> configuration.
//
#pragma once



#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))



#line 21 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"




#line 26 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"
    
#line 28 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"


    

#line 33 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"
        
    

#line 37 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"
#line 38 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"



// Predefine _CRT_STDIO_ISO_WIDE_SPECIFIERS to use ISO-conforming behavior for
// the wide string printf and scanf functions (%s, %c, and %[] specifiers).
//
// Predefine _CRT_STDIO_LEGACY_WIDE_SPECIFIERS to use VC++ 2013 and earlier behavior for
// the wide string printf and scanf functions (%s, %c, and %[] specifiers).
//
// Predefine _CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS when building code that does
// not use these format specifiers without a length modifier and thus can be
// used with either the legacy (default) or the conforming mode.  (This option
// is intended for use by static libraries).

    








#line 62 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"




#line 67 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"

    





#line 75 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"
#line 76 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"

// If we're compiling mixed managed code, make sure these inline functions are
// compiled as native to ensure that there is only one instance of each of the
// function-local static variables.


#line 83 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"


    // This function must not be inlined into callers to avoid ODR violations.  The
    // static local variable has different names in C and in C++ translation units.
       
    
    __declspec(noinline) __inline unsigned __int64* __cdecl __local_stdio_printf_options(void)
    {
        static unsigned __int64 _OptionsStorage;
        return &_OptionsStorage;
    }

    // This function must not be inlined into callers to avoid ODR violations.  The
    // static local variable has different names in C and in C++ translation units.
       
    
    __declspec(noinline) __inline unsigned __int64* __cdecl __local_stdio_scanf_options(void)
    {
        static unsigned __int64 _OptionsStorage;
        return &_OptionsStorage;
    }
#line 105 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"



#line 109 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_stdio_config.h"




















__pragma(pack(pop))

#pragma warning(pop) 
#line 14 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))

//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Stream I/O Declarations Required by this Header
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    
    typedef struct _iobuf
    {
        void* _Placeholder;
    } FILE;
#line 33 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

 FILE* __cdecl __acrt_iob_func(unsigned _Ix);










    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Wide Character Stream I/O Functions
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
     wint_t __cdecl fgetwc(
          FILE* _Stream
        );

    
     wint_t __cdecl _fgetwchar(void);

    
     wint_t __cdecl fputwc(
             wchar_t _Character,
          FILE*   _Stream);

    
     wint_t __cdecl _fputwchar(
          wchar_t _Character
        );

     
     wint_t __cdecl getwc(
          FILE* _Stream
        );

     
     wint_t __cdecl getwchar(void);


    
     
     wchar_t* __cdecl fgetws(
          wchar_t* _Buffer,
                                  int      _BufferCount,
                               FILE*    _Stream
        );

    
     int __cdecl fputws(
           wchar_t const* _Buffer,
          FILE*          _Stream
        );

    
     
     wchar_t* __cdecl _getws_s(
          wchar_t* _Buffer,
                                  size_t   _BufferCount
        );

    
#line 103 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
     wint_t __cdecl putwc(
             wchar_t _Character,
          FILE*   _Stream
        );

    
     wint_t __cdecl putwchar(
          wchar_t _Character
        );

    
     int __cdecl _putws(
          wchar_t const* _Buffer
        );

    
     wint_t __cdecl ungetwc(
             wint_t _Character,
          FILE*  _Stream
        );

     
     FILE * __cdecl _wfdopen(
            int            _FileHandle,
          wchar_t const* _Mode
        );

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wfopen_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     FILE* __cdecl _wfopen(
          wchar_t const* _FileName,
          wchar_t const* _Mode
        );

    
     errno_t __cdecl _wfopen_s(
          FILE**         _Stream,
                             wchar_t const* _FileName,
                             wchar_t const* _Mode
        );

     
    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wfreopen_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     FILE* __cdecl _wfreopen(
           wchar_t const* _FileName,
           wchar_t const* _Mode,
          FILE*          _OldStream
        );

    
     errno_t __cdecl _wfreopen_s(
          FILE**         _Stream,
                             wchar_t const* _FileName,
                             wchar_t const* _Mode,
                            FILE*          _OldStream
        );

     
     FILE* __cdecl _wfsopen(
          wchar_t const* _FileName,
          wchar_t const* _Mode,
            int            _ShFlag
        );

     void __cdecl _wperror(
          wchar_t const* _ErrorMessage
        );

    

         
         FILE* __cdecl _wpopen(
              wchar_t const* _Command,
              wchar_t const* _Mode
            );

    #line 181 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     int __cdecl _wremove(
          wchar_t const* _FileName
        );

    
    

     
     __declspec(allocator) wchar_t* __cdecl _wtempnam(
          wchar_t const* _Directory,
          wchar_t const* _FilePrefix
        );

    

     
    
     errno_t __cdecl _wtmpnam_s(
          wchar_t* _Buffer,
                                  size_t   _BufferCount
        );

    
#line 209 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wtmpnam_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   wchar_t* __cdecl _wtmpnam(  wchar_t *_Buffer);
#line 215 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"



    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // I/O Synchronization and _nolock family of I/O functions
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
     wint_t __cdecl _fgetwc_nolock(
          FILE* _Stream
        );

    
     wint_t __cdecl _fputwc_nolock(
             wchar_t _Character,
          FILE*   _Stream
        );

    
     wint_t __cdecl _getwc_nolock(
          FILE* _Stream
        );

    
     wint_t __cdecl _putwc_nolock(
             wchar_t _Character,
          FILE*   _Stream
        );

    
     wint_t __cdecl _ungetwc_nolock(
             wint_t _Character,
          FILE*  _Stream
        );

    



#line 256 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"



    // Variadic functions are not supported in managed code under /clr
    





    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Wide Character Formatted Output Functions (Stream)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
     int __cdecl __stdio_common_vfwprintf(
                                             unsigned __int64 _Options,
                                          FILE*            _Stream,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

    
     int __cdecl __stdio_common_vfwprintf_s(
                                             unsigned __int64 _Options,
                                          FILE*            _Stream,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

    
     int __cdecl __stdio_common_vfwprintf_p(
                                             unsigned __int64 _Options,
                                          FILE*            _Stream,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

    
    __inline int __cdecl _vfwprintf_l(
                                          FILE*          const _Stream,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 308 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vfwprintf((*__local_stdio_printf_options()), _Stream, _Format, _Locale, _ArgList);
    }
    #line 312 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl vfwprintf(
                                FILE*          const _Stream,
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 322 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_l(_Stream, _Format, ((void *)0), _ArgList);
    }
    #line 326 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vfwprintf_s_l(
                                          FILE*          const _Stream,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 337 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vfwprintf_s((*__local_stdio_printf_options()), _Stream, _Format, _Locale, _ArgList);
    }
    #line 341 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl vfwprintf_s(
                                    FILE*          const _Stream,
                wchar_t const* const _Format,
                                          va_list              _ArgList
            )
    

#line 353 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            return _vfwprintf_s_l(_Stream, _Format, ((void *)0), _ArgList);
        }
    #line 357 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 359 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vfwprintf_p_l(
                                          FILE*          const _Stream,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 370 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vfwprintf_p((*__local_stdio_printf_options()), _Stream, _Format, _Locale, _ArgList);
    }
    #line 374 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vfwprintf_p(
                                FILE*          const _Stream,
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 384 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_p_l(_Stream, _Format, ((void *)0), _ArgList);
    }
    #line 388 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vwprintf_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 398 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
    }
    #line 402 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl vwprintf(
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 411 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
    }
    #line 415 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vwprintf_s_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 425 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_s_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
    }
    #line 429 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl vwprintf_s(
                wchar_t const* const _Format,
                                          va_list              _ArgList
            )
    

#line 440 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            return _vfwprintf_s_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
        }
    #line 444 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 446 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vwprintf_p_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 456 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_p_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
    }
    #line 460 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vwprintf_p(
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 469 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwprintf_p_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
    }
    #line 473 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _fwprintf_l(
                                          FILE*          const _Stream,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 483 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwprintf_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 492 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl fwprintf(
                                FILE*          const _Stream,
            wchar_t const* const _Format,
        ...)
    

#line 501 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfwprintf_l(_Stream, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 510 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _fwprintf_s_l(
                                          FILE*          const _Stream,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 520 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwprintf_s_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 529 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl fwprintf_s(
                                    FILE*          const _Stream,
                wchar_t const* const _Format,
            ...)
    

#line 540 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfwprintf_s_l(_Stream, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 549 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 551 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _fwprintf_p_l(
                                          FILE*          const _Stream,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 561 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwprintf_p_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 570 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _fwprintf_p(
                                FILE*          const _Stream,
            wchar_t const* const _Format,
        ...)
    

#line 579 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfwprintf_p_l(_Stream, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 588 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _wprintf_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 597 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwprintf_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 606 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl wprintf(
            wchar_t const* const _Format,
        ...)
    

#line 614 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfwprintf_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 623 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _wprintf_s_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 632 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwprintf_s_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 641 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl wprintf_s(
                wchar_t const* const _Format,
            ...)
    

#line 651 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfwprintf_s_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 660 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 662 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _wprintf_p_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 671 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwprintf_p_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 680 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _wprintf_p(
            wchar_t const* const _Format,
        ...)
    

#line 688 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfwprintf_p_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 697 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"


    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Wide Character Formatted Input Functions (Stream)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
     int __cdecl __stdio_common_vfwscanf(
                                            unsigned __int64 _Options,
                                         FILE*            _Stream,
            wchar_t const*   _Format,
                                        _locale_t        _Locale,
                                               va_list          _ArgList
        );

    
    __inline int __cdecl _vfwscanf_l(
          FILE*                                const _Stream,
            wchar_t const* const _Format,
                               _locale_t      const _Locale,
                                      va_list              _ArgList
        )
    

#line 723 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vfwscanf(
            (*__local_stdio_scanf_options ()),
            _Stream, _Format, _Locale, _ArgList);
    }
    #line 729 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl vfwscanf(
          FILE*                                const _Stream,
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 739 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwscanf_l(_Stream, _Format, ((void *)0), _ArgList);
    }
    #line 743 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vfwscanf_s_l(
                                FILE*          const _Stream,
            wchar_t const* const _Format,
                               _locale_t      const _Locale,
                                      va_list              _ArgList
        )
    

#line 754 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vfwscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Stream, _Format, _Locale, _ArgList);
    }
    #line 760 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl vfwscanf_s(
                                    FILE*          const _Stream,
                wchar_t const* const _Format,
                                          va_list              _ArgList
            )
    

#line 772 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            return _vfwscanf_s_l(_Stream, _Format, ((void *)0), _ArgList);
        }
    #line 776 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 778 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    __inline int __cdecl _vwscanf_l(
            wchar_t const* const _Format,
                               _locale_t      const _Locale,
                                      va_list              _ArgList
        )
    

#line 787 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwscanf_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
    }
    #line 791 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl vwscanf(
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 800 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwscanf_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
    }
    #line 804 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _vwscanf_s_l(
            wchar_t const* const _Format,
                               _locale_t      const _Locale,
                                      va_list              _ArgList
        )
    

#line 814 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vfwscanf_s_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
    }
    #line 818 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl vwscanf_s(
                wchar_t const* const _Format,
                                          va_list              _ArgList
            )
    

#line 829 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            return _vfwscanf_s_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
        }
    #line 833 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 835 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_fwscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _fwscanf_l(
                                         FILE*          const _Stream,
            wchar_t const* const _Format,
                                        _locale_t      const _Locale,
        ...)
    

#line 845 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwscanf_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 854 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "fwscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl fwscanf(
                               FILE*          const _Stream,
            wchar_t const* const _Format,
        ...)
    

#line 863 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfwscanf_l(_Stream, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 872 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _fwscanf_s_l(
                                           FILE*          const _Stream,
            wchar_t const* const _Format,
                                          _locale_t      const _Locale,
        ...)
    

#line 882 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwscanf_s_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 891 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl fwscanf_s(
                                     FILE*          const _Stream,
                wchar_t const* const _Format,
            ...)
    

#line 902 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfwscanf_s_l(_Stream, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 911 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 913 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _wscanf_l(
            wchar_t const* const _Format,
                                        _locale_t      const _Locale,
        ...)
    

#line 922 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwscanf_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 931 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "wscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl wscanf(
            wchar_t const* const _Format,
        ...)
    

#line 939 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfwscanf_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 948 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
    __inline int __cdecl _wscanf_s_l(
            wchar_t const* const _Format,
                                          _locale_t      const _Locale,
        ...)
    

#line 957 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfwscanf_s_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 966 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

        
        __inline int __cdecl wscanf_s(
                wchar_t const* const _Format,
            ...)
    

#line 976 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfwscanf_s_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 985 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 987 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"



    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Wide Character Formatted Output Functions (String)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    
        



    

#line 1003 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
     int __cdecl __stdio_common_vswprintf(
                                             unsigned __int64 _Options,
                 wchar_t*         _Buffer,
                                             size_t           _BufferCount,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
    
     int __cdecl __stdio_common_vswprintf_s(
                                             unsigned __int64 _Options,
                     wchar_t*         _Buffer,
                                             size_t           _BufferCount,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
    
     int __cdecl __stdio_common_vsnwprintf_s(
                                             unsigned __int64 _Options,
                 wchar_t*         _Buffer,
                                             size_t           _BufferCount,
                                             size_t           _MaxCount,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
    
     int __cdecl __stdio_common_vswprintf_p(
                                             unsigned __int64 _Options,
                     wchar_t*         _Buffer,
                                             size_t           _BufferCount,
            wchar_t const*   _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsnwprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _vsnwprintf_l(
            wchar_t*       const _Buffer,
                                                  size_t         const _BufferCount,
                 wchar_t const* const _Format,
                                              _locale_t      const _Locale,
                                                     va_list              _ArgList
        )
    

#line 1061 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vswprintf(
            (*__local_stdio_printf_options()) | (1ULL << 0),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1069 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vsnwprintf_s_l(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                                       size_t         const _MaxCount,
                      wchar_t const* const _Format,
                                                   _locale_t      const _Locale,
                                                          va_list              _ArgList
        )
    

#line 1083 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vsnwprintf_s(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _MaxCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1091 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vsnwprintf_s(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                                       size_t         const _MaxCount,
                                wchar_t const* const _Format,
                                                          va_list              _ArgList
        )
    

#line 1104 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vsnwprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, ((void *)0), _ArgList);
    }
    #line 1108 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snwprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) __inline   int __cdecl _snwprintf(    wchar_t *_Buffer,   size_t _BufferCount,     wchar_t const* _Format, ...); __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsnwprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) __inline   int __cdecl _vsnwprintf(    wchar_t *_Buffer,   size_t _BufferCount,     wchar_t const* _Format, va_list _Args);
#line 1117 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsnwprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _vsnwprintf(
            wchar_t*       _Buffer,
                                                  size_t         _BufferCount,
                           wchar_t const* _Format,
                                                     va_list        _ArgList
        )
    

#line 1129 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vsnwprintf_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1133 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
#line 1142 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf_c_l(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                      wchar_t const* const _Format,
                                                   _locale_t      const _Locale,
                                                          va_list              _ArgList
        )
    

#line 1155 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vswprintf(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1163 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf_c(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                wchar_t const* const _Format,
                                                          va_list              _ArgList
        )
    

#line 1175 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswprintf_c_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1179 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf_l(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                      wchar_t const* const _Format,
                                                   _locale_t      const _Locale,
                                                          va_list              _ArgList
        )
    

#line 1192 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswprintf_c_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
    }
    #line 1196 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl __vswprintf_l(
                  wchar_t*       const _Buffer,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 1208 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswprintf_l(_Buffer, (size_t)-1, _Format, _Locale, _ArgList);
    }
    #line 1212 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf(
           wchar_t*       const _Buffer,
               wchar_t const* const _Format,
                                         va_list              _ArgList
        )
    

#line 1223 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswprintf_l(_Buffer, (size_t)-1, _Format, ((void *)0), _ArgList);
    }
    #line 1227 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl vswprintf(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                      wchar_t const* const _Format,
                                                          va_list              _ArgList
        )
    

#line 1239 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswprintf_c_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1243 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf_s_l(
           wchar_t*       const _Buffer,
                                                   size_t         const _BufferCount,
                  wchar_t const* const _Format,
                                               _locale_t      const _Locale,
                                                      va_list              _ArgList
        )
    

#line 1256 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vswprintf_s(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1264 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

         
        __inline int __cdecl vswprintf_s(
               wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                wchar_t const* const _Format,
                                                          va_list              _ArgList
            )
    

#line 1277 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            return _vswprintf_s_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        }
    #line 1281 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 1283 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
#line 1291 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf_p_l(
           wchar_t*       const _Buffer,
                                                   size_t         const _BufferCount,
                  wchar_t const* const _Format,
                                               _locale_t      const _Locale,
                                                      va_list              _ArgList
        )
    

#line 1304 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vswprintf_p(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1312 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswprintf_p(
           wchar_t*       const _Buffer,
                                                   size_t         const _BufferCount,
                            wchar_t const* const _Format,
                                                      va_list              _ArgList
        )
    

#line 1324 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswprintf_p_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1328 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _vscwprintf_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 1339 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vswprintf(
            (*__local_stdio_printf_options()) | (1ULL << 1),
            ((void *)0), 0, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1347 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _vscwprintf(
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 1357 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vscwprintf_l(_Format, ((void *)0), _ArgList);
    }
    #line 1361 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _vscwprintf_p_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
                                                va_list              _ArgList
        )
    

#line 1372 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int const _Result = __stdio_common_vswprintf_p(
            (*__local_stdio_printf_options()) | (1ULL << 1),
            ((void *)0), 0, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1380 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _vscwprintf_p(
            wchar_t const* const _Format,
                                      va_list              _ArgList
        )
    

#line 1390 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vscwprintf_p_l(_Format, ((void *)0), _ArgList);
    }
    #line 1394 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl __swprintf_l(
                  wchar_t*       const _Buffer,
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 1405 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = __vswprintf_l(_Buffer, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1414 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf_l(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                      wchar_t const* const _Format,
                                                   _locale_t      const _Locale,
        ...)
    

#line 1426 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vswprintf_c_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1435 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf(
           wchar_t*       const _Buffer,
               wchar_t const* const _Format,
        ...)
    

#line 1445 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = __vswprintf_l(_Buffer, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1454 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl swprintf(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                wchar_t const* const _Format,
        ...)
    

#line 1465 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vswprintf_c_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1474 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "__swprintf_l_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) __inline   int __cdecl __swprintf_l(   wchar_t *_Buffer,     wchar_t const* _Format,   _locale_t _Locale, ...); __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vswprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) __inline   int __cdecl __vswprintf_l(   wchar_t *_Buffer,     wchar_t const* _Format,   _locale_t _Locale, va_list _Args);
#line 1483 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "swprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) __inline   int __cdecl _swprintf(   wchar_t *_Buffer,     wchar_t const* _Format, ...); __declspec(deprecated("This function or variable may be unsafe. Consider using " "vswprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details.")) __inline   int __cdecl _vswprintf(   wchar_t *_Buffer,     wchar_t const* _Format, va_list _Args);
#line 1490 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf_s_l(
           wchar_t*       const _Buffer,
                                                   size_t         const _BufferCount,
                  wchar_t const* const _Format,
                                               _locale_t      const _Locale,
        ...)
    

#line 1502 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vswprintf_s_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1511 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

         
        __inline int __cdecl swprintf_s(
               wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                wchar_t const* const _Format,
            ...)
    

#line 1523 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vswprintf_s_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 1532 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 1534 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
#line 1541 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf_p_l(
           wchar_t*       const _Buffer,
                                                   size_t         const _BufferCount,
                  wchar_t const* const _Format,
                                               _locale_t      const _Locale,
        ...)
    

#line 1553 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vswprintf_p_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1562 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf_p(
           wchar_t*       const _Buffer,
                                                   size_t         const _BufferCount,
                            wchar_t const* const _Format,
        ...)
    

#line 1573 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vswprintf_p_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1582 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf_c_l(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                      wchar_t const* const _Format,
                                                   _locale_t      const _Locale,
        ...)
    

#line 1594 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vswprintf_c_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1603 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swprintf_c(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                wchar_t const* const _Format,
        ...)
    

#line 1614 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vswprintf_c_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1623 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snwprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _snwprintf_l(
            wchar_t*       const _Buffer,
                                                  size_t         const _BufferCount,
                 wchar_t const* const _Format,
                                              _locale_t      const _Locale,
        ...)
    

#line 1635 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));

        _Result = _vsnwprintf_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1646 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _snwprintf(
            wchar_t*       _Buffer,
                                                  size_t         _BufferCount,
                           wchar_t const* _Format,
        ...)
    

#line 1657 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));

        _Result = _vsnwprintf_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1668 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _snwprintf_s_l(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                                       size_t         const _MaxCount,
                      wchar_t const* const _Format,
                                                   _locale_t      const _Locale,
        ...)
    

#line 1681 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsnwprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1690 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _snwprintf_s(
           wchar_t*       const _Buffer,
                                                       size_t         const _BufferCount,
                                                       size_t         const _MaxCount,
                                wchar_t const* const _Format,
        ...)
    

#line 1702 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsnwprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1711 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
#line 1719 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    __inline int __cdecl _scwprintf_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 1728 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vscwprintf_l(_Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1737 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _scwprintf(
            wchar_t const* const _Format,
        ...)
    

#line 1746 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vscwprintf_l(_Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1755 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _scwprintf_p_l(
            wchar_t const* const _Format,
                                         _locale_t      const _Locale,
        ...)
    

#line 1765 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vscwprintf_p_l(_Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1774 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     
    __inline int __cdecl _scwprintf_p(
            wchar_t const* const _Format,
        ...)
    

#line 1783 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vscwprintf_p_l(_Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1792 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"


    
        // C4141: double deprecation
        // C6054: string may not be zero-terminated
        #pragma warning(push)
        #pragma warning(disable: 4141 6054)

        





















































        #pragma warning(pop)
    #line 1856 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    




#line 1863 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"


    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Wide Character Formatted Input Functions (String)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     
     int __cdecl __stdio_common_vswscanf(
                                            unsigned __int64 _Options,
                  wchar_t const*   _Buffer,
                                            size_t           _BufferCount,
            wchar_t const*   _Format,
                                        _locale_t        _Locale,
                                               va_list          _ArgList
        );

     
    
    __inline int __cdecl _vswscanf_l(
                                 wchar_t const* const _Buffer,
            wchar_t const* const _Format,
                               _locale_t      const _Locale,
                                      va_list              _ArgList
        )
    

#line 1891 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vswscanf(
            (*__local_stdio_scanf_options ()),
            _Buffer, (size_t)-1, _Format, _Locale, _ArgList);
    }
    #line 1897 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl vswscanf(
                                 wchar_t const* _Buffer,
            wchar_t const* _Format,
                                      va_list        _ArgList
        )
    

#line 1908 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return _vswscanf_l(_Buffer, _Format, ((void *)0), _ArgList);
    }
    #line 1912 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vswscanf_s_l(
                                 wchar_t const* const _Buffer,
            wchar_t const* const _Format,
                               _locale_t      const _Locale,
                                      va_list              _ArgList
        )
    

#line 1924 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vswscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Buffer, (size_t)-1, _Format, _Locale, _ArgList);
    }
    #line 1930 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

         
        
        __inline int __cdecl vswscanf_s(
                                     wchar_t const* const _Buffer,
                wchar_t const* const _Format,
                                          va_list              _ArgList
            )
    

#line 1943 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            return _vswscanf_s_l(_Buffer, _Format, ((void *)0), _ArgList);
        }
    #line 1947 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 1949 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    
#line 1957 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsnwscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _vsnwscanf_l(
                  wchar_t const* const _Buffer,
                                            size_t         const _BufferCount,
            wchar_t const* const _Format,
                                        _locale_t      const _Locale,
                                               va_list              _ArgList
        )
    

#line 1970 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vswscanf(
            (*__local_stdio_scanf_options ()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);
    }
    #line 1976 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _vsnwscanf_s_l(
                    wchar_t const* const _Buffer,
                                              size_t         const _BufferCount,
            wchar_t const* const _Format,
                                          _locale_t      const _Locale,
                                                 va_list              _ArgList
        )
    

#line 1989 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        return __stdio_common_vswscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);
    }
    #line 1995 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_swscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _swscanf_l(
                                          wchar_t const* const _Buffer,
            wchar_t const* const _Format,
                                        _locale_t            _Locale,
        ...)
    

#line 2006 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vswscanf_l(_Buffer, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2015 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
      __declspec(deprecated("This function or variable may be unsafe. Consider using " "swscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl swscanf(
                                wchar_t const* const _Buffer,
            wchar_t const* const _Format,
        ...)
    

#line 2025 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vswscanf_l(_Buffer, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2034 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _swscanf_s_l(
                                            wchar_t const* const _Buffer,
            wchar_t const* const _Format,
                                          _locale_t      const _Locale,
        ...)
    

#line 2045 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vswscanf_s_l(_Buffer, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2054 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    

         
        
        __inline int __cdecl swscanf_s(
                                      wchar_t const* const _Buffer,
                wchar_t const* const _Format,
            ...)
    

#line 2066 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vswscanf_s_l(_Buffer, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 2075 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    #line 2077 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snwscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _snwscanf_l(
                  wchar_t const* const _Buffer,
                                            size_t         const _BufferCount,
            wchar_t const* const _Format,
                                        _locale_t      const _Locale,
        ...)
    

#line 2089 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));

        _Result = _vsnwscanf_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2100 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snwscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _snwscanf(
            wchar_t const* const _Buffer,
                                      size_t         const _BufferCount,
                wchar_t const* const _Format,
        ...)
    

#line 2111 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));

        _Result = _vsnwscanf_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2122 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _snwscanf_s_l(
                    wchar_t const* const _Buffer,
                                              size_t         const _BufferCount,
            wchar_t const* const _Format,
                                          _locale_t      const _Locale,
        ...)
    

#line 2134 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsnwscanf_s_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2143 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

     
    
    __inline int __cdecl _snwscanf_s(
             wchar_t const* const _Buffer,
                                       size_t         const _BufferCount,
               wchar_t const* const _Format,
        ...)
    

#line 2154 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsnwscanf_s_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2163 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

    


#line 2168 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wstdio.h"

__pragma(pack(pop))

#pragma warning(pop) 
#line 14 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))

/* Buffered I/O macros */





/*
 * Default number of supported streams. _NFILE is confusing and obsolete, but
 * supported anyway for backwards compatibility.
 */




/*
 * Number of entries in _iob[] (declared below). Note that _NSTREAM_ must be
 * greater than or equal to _IOB_ENTRIES.
 */












    
#line 53 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"



/* Seek method constants */











    
    
#line 71 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"


typedef __int64 fpos_t;





    
     errno_t __cdecl _get_stream_buffer_pointers(
               FILE*   _Stream,
          char*** _Base,
          char*** _Pointer,
          int**   _Count
        );


    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Narrow Character Stream I/O Functions
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    

        
         errno_t __cdecl clearerr_s(
              FILE* _Stream
            );

        
         
         errno_t __cdecl fopen_s(
              FILE**      _Stream,
                                     char const* _FileName,
                                     char const* _Mode
            );

        
         
         size_t __cdecl fread_s(
                void*  _Buffer,
                                    size_t _BufferSize,
                                                                            size_t _ElementSize,
                                                                            size_t _ElementCount,
                                                                         FILE*  _Stream
            );

        
         errno_t __cdecl freopen_s(
              FILE**      _Stream,
                                 char const* _FileName,
                                 char const* _Mode,
                                FILE*       _OldStream
            );

         
         char* __cdecl gets_s(
              char*   _Buffer,
                               rsize_t _Size
            );

        
         errno_t __cdecl tmpfile_s(
                FILE** _Stream
            );

         
        
         errno_t __cdecl tmpnam_s(
              char*   _Buffer,
                               rsize_t _Size
            );

    #line 145 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     void __cdecl clearerr(
          FILE* _Stream
        );

     
    
     int __cdecl fclose(
          FILE* _Stream
        );

    
     int __cdecl _fcloseall(void);

     
     FILE* __cdecl _fdopen(
            int         _FileHandle,
          char const* _Mode
        );

     
     int __cdecl feof(
          FILE* _Stream
        );

     
     int __cdecl ferror(
          FILE* _Stream
        );

    
     int __cdecl fflush(
          FILE* _Stream
        );

     
    
     int __cdecl fgetc(
          FILE* _Stream
        );

    
     int __cdecl _fgetchar(void);

     
    
     int __cdecl fgetpos(
          FILE*   _Stream,
            fpos_t* _Position
        );

     
    
     char* __cdecl fgets(
          char* _Buffer,
                               int   _MaxCount,
                            FILE* _Stream
        );

     
     int __cdecl _fileno(
          FILE* _Stream
        );

    
     int __cdecl _flushall(void);

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "fopen_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     FILE* __cdecl fopen(
          char const* _FileName,
          char const* _Mode
        );


     
    
     int __cdecl fputc(
             int   _Character,
          FILE* _Stream
        );

    
     int __cdecl _fputchar(
          int _Character
        );

     
    
     int __cdecl fputs(
           char const* _Buffer,
          FILE*       _Stream
        );

    
     size_t __cdecl fread(
          void*  _Buffer,
                                                      size_t _ElementSize,
                                                      size_t _ElementCount,
                                                   FILE*  _Stream
        );

     
      __declspec(deprecated("This function or variable may be unsafe. Consider using " "freopen_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     FILE* __cdecl freopen(
           char const* _FileName,
           char const* _Mode,
          FILE*       _Stream
        );

     
     FILE* __cdecl _fsopen(
          char const* _FileName,
          char const* _Mode,
            int         _ShFlag
        );

     
    
     int __cdecl fsetpos(
          FILE*         _Stream,
             fpos_t const* _Position
        );

     
    
     int __cdecl fseek(
          FILE* _Stream,
             long  _Offset,
             int   _Origin
        );

     
    
     int __cdecl _fseeki64(
          FILE*   _Stream,
             __int64 _Offset,
             int     _Origin
        );

     
     
     long __cdecl ftell(
          FILE* _Stream
        );

     
     
     __int64 __cdecl _ftelli64(
          FILE* _Stream
        );

    
     size_t __cdecl fwrite(
          void const* _Buffer,
                                                    size_t      _ElementSize,
                                                    size_t      _ElementCount,
                                                 FILE*       _Stream
        );

     
     
     int __cdecl getc(
          FILE* _Stream
        );

     
     int __cdecl getchar(void);

     
     int __cdecl _getmaxstdio(void);

    
#line 319 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
     int __cdecl _getw(
          FILE* _Stream
        );

     void __cdecl perror(
          char const* _ErrorMessage
        );

    

         
        
         int __cdecl _pclose(
              FILE* _Stream
            );

         
         FILE* __cdecl _popen(
              char const* _Command,
              char const* _Mode
            );

    #line 344 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
     int __cdecl putc(
             int   _Character,
          FILE* _Stream
        );

    
     int __cdecl putchar(
          int _Character
        );

    
     int __cdecl puts(
          char const* _Buffer
        );

     
    
     int __cdecl _putw(
             int   _Word,
          FILE* _Stream
        );

     int __cdecl remove(
          char const* _FileName
        );

     
     int __cdecl rename(
          char const* _OldFileName,
          char const* _NewFileName
        );

     int __cdecl _unlink(
          char const* _FileName
        );

    

        __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_unlink" ". See online help for details."))
         int __cdecl unlink(
              char const* _FileName
            );

    #line 391 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     void __cdecl rewind(
          FILE* _Stream
        );

    
     int __cdecl _rmtmp(void);

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "setvbuf" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     void __cdecl setbuf(
                                                      FILE* _Stream,
            char* _Buffer
        );

    
     int __cdecl _setmaxstdio(
          int _Maximum
        );

     
    
     int __cdecl setvbuf(
                               FILE*  _Stream,
            char*  _Buffer,
                                  int    _Mode,
                                  size_t _Size
        );

    


#line 423 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
     __declspec(allocator) char* __cdecl _tempnam(
          char const* _DirectoryName,
          char const* _FilePrefix
        );

    

#line 433 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "tmpfile_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
     FILE* __cdecl tmpfile(void);

    
#line 442 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "tmpnam_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   char* __cdecl tmpnam(  char *_Buffer);
#line 448 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
     int __cdecl ungetc(
             int   _Character,
          FILE* _Stream
        );



    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // I/O Synchronization and _nolock family of I/O functions
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     void __cdecl _lock_file(
          FILE* _Stream
        );

     void __cdecl _unlock_file(
          FILE* _Stream
        );

     
    
     int __cdecl _fclose_nolock(
          FILE* _Stream
        );

     
    
     int __cdecl _fflush_nolock(
          FILE* _Stream
        );

     
    
     int __cdecl _fgetc_nolock(
          FILE* _Stream
        );

     
    
     int __cdecl _fputc_nolock(
             int   _Character,
          FILE* _Stream
        );

    
     size_t __cdecl _fread_nolock(
          void*  _Buffer,
                                                      size_t _ElementSize,
                                                      size_t _ElementCount,
                                                   FILE*  _Stream
        );

    
     
     size_t __cdecl _fread_nolock_s(
          void*  _Buffer,
                              size_t _BufferSize,
                                                                      size_t _ElementSize,
                                                                      size_t _ElementCount,
                                                                   FILE*  _Stream
        );

    
     int __cdecl _fseek_nolock(
          FILE* _Stream,
             long  _Offset,
             int   _Origin
        );

    
     int __cdecl _fseeki64_nolock(
          FILE*   _Stream,
             __int64 _Offset,
             int     _Origin
        );

     
     long __cdecl _ftell_nolock(
          FILE* _Stream
        );

     
     __int64 __cdecl _ftelli64_nolock(
          FILE* _Stream
        );

    
     size_t __cdecl _fwrite_nolock(
          void const* _Buffer,
                                                    size_t      _ElementSize,
                                                    size_t      _ElementCount,
                                                 FILE*       _Stream
        );

    
     int __cdecl _getc_nolock(
          FILE* _Stream
        );

    
     int __cdecl _putc_nolock(
             int   _Character,
          FILE* _Stream
        );

    
     int __cdecl _ungetc_nolock(
             int   _Character,
          FILE* _Stream
        );

    
    
    
    



    














#line 586 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"



     int* __cdecl __p__commode(void);

    


        
    #line 596 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"



    // Variadic functions are not supported in managed code under /clr
    

#line 603 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Narrow Character Formatted Output Functions (Stream)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     int __cdecl __stdio_common_vfprintf(
                                             unsigned __int64 _Options,
                                          FILE*            _Stream,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     int __cdecl __stdio_common_vfprintf_s(
                                             unsigned __int64 _Options,
                                          FILE*            _Stream,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
     int __cdecl __stdio_common_vfprintf_p(
                                             unsigned __int64 _Options,
                                          FILE*            _Stream,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

    
    __inline int __cdecl _vfprintf_l(
           FILE*       const _Stream,
            char const* const _Format,
          _locale_t   const _Locale,
                 va_list           _ArgList
        )
    

#line 644 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vfprintf((*__local_stdio_printf_options()), _Stream, _Format, _Locale, _ArgList);
    }
    #line 648 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl vfprintf(
                                FILE*       const _Stream,
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 658 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_l(_Stream, _Format, ((void *)0), _ArgList);
    }
    #line 662 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vfprintf_s_l(
           FILE*       const _Stream,
            char const* const _Format,
          _locale_t   const _Locale,
                 va_list           _ArgList
        )
    

#line 673 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vfprintf_s((*__local_stdio_printf_options()), _Stream, _Format, _Locale, _ArgList);
    }
    #line 677 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl vfprintf_s(
                                    FILE*       const _Stream,
                char const* const _Format,
                                          va_list           _ArgList
            )
    

#line 689 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vfprintf_s_l(_Stream, _Format, ((void *)0), _ArgList);
        }
    #line 693 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 695 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vfprintf_p_l(
           FILE*       const _Stream,
            char const* const _Format,
          _locale_t   const _Locale,
                 va_list           _ArgList
        )
    

#line 706 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vfprintf_p((*__local_stdio_printf_options()), _Stream, _Format, _Locale, _ArgList);
    }
    #line 710 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vfprintf_p(
                                FILE*       const _Stream,
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 720 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_p_l(_Stream, _Format, ((void *)0), _ArgList);
    }
    #line 724 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vprintf_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
                                                va_list           _ArgList
        )
    

#line 734 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
    }
    #line 738 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl vprintf(
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 747 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
    }
    #line 751 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vprintf_s_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
                                                va_list           _ArgList
        )
    

#line 761 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_s_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
    }
    #line 765 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl vprintf_s(
                char const* const _Format,
                                          va_list           _ArgList
            )
    

#line 776 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vfprintf_s_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
        }
    #line 780 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 782 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vprintf_p_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
                                                va_list           _ArgList
        )
    

#line 792 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_p_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
    }
    #line 796 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vprintf_p(
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 805 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfprintf_p_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
    }
    #line 809 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _fprintf_l(
                                          FILE*       const _Stream,
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 819 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfprintf_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 828 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl fprintf(
                                FILE*       const _Stream,
            char const* const _Format,
        ...)
    

#line 837 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfprintf_l(_Stream, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 846 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     int __cdecl _set_printf_count_output(
          int _Value
        );

     int __cdecl _get_printf_count_output(void);

    
    __inline int __cdecl _fprintf_s_l(
                                          FILE*       const _Stream,
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 862 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfprintf_s_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 871 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl fprintf_s(
                                    FILE*       const _Stream,
                char const* const _Format,
            ...)
    

#line 882 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfprintf_s_l(_Stream, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 891 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 893 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _fprintf_p_l(
                                          FILE*       const _Stream,
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 903 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfprintf_p_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 912 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _fprintf_p(
                                FILE*       const _Stream,
            char const* const _Format,
        ...)
    

#line 921 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfprintf_p_l(_Stream, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 930 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _printf_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 939 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfprintf_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 948 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl printf(
            char const* const _Format,
        ...)
    

#line 956 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfprintf_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 965 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _printf_s_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 974 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfprintf_s_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 983 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl printf_s(
                char const* const _Format,
            ...)
    

#line 993 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfprintf_s_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 1002 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1004 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _printf_p_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 1013 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfprintf_p_l((__acrt_iob_func(1)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1022 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _printf_p(
            char const* const _Format,
        ...)
    

#line 1030 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfprintf_p_l((__acrt_iob_func(1)), _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1039 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"


    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Narrow Character Formatted Input Functions (Stream)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     int __cdecl __stdio_common_vfscanf(
                                            unsigned __int64 _Options,
                                         FILE*            _Stream,
            char const*      _Format,
                                        _locale_t        _Locale,
                                               va_list          _Arglist
        );

    
    __inline int __cdecl _vfscanf_l(
                                FILE*       const _Stream,
            char const* const _Format,
                               _locale_t   const _Locale,
                                      va_list           _ArgList
        )
    

#line 1064 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vfscanf(
            (*__local_stdio_scanf_options ()),
            _Stream, _Format, _Locale, _ArgList);
    }
    #line 1070 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl vfscanf(
                                FILE*       const _Stream,
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 1080 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfscanf_l(_Stream, _Format, ((void *)0), _ArgList);
    }
    #line 1084 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vfscanf_s_l(
                                FILE*       const _Stream,
            char const* const _Format,
                               _locale_t   const _Locale,
                                      va_list           _ArgList
        )
    

#line 1095 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vfscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Stream, _Format, _Locale, _ArgList);
    }
    #line 1101 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"


    

        
        __inline int __cdecl vfscanf_s(
                                    FILE*       const _Stream,
                char const* const _Format,
                                          va_list           _ArgList
            )
    

#line 1114 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vfscanf_s_l(_Stream, _Format, ((void *)0), _ArgList);
        }
    #line 1118 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1120 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vscanf_l(
            char const* const _Format,
                               _locale_t   const _Locale,
                                      va_list           _ArgList
        )
    

#line 1130 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfscanf_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
    }
    #line 1134 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl vscanf(
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 1143 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfscanf_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
    }
    #line 1147 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vscanf_s_l(
            char const* const _Format,
                               _locale_t   const _Locale,
                                      va_list           _ArgList
        )
    

#line 1157 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vfscanf_s_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
    }
    #line 1161 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl vscanf_s(
                char const* const _Format,
                                          va_list           _ArgList
            )
    

#line 1172 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vfscanf_s_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
        }
    #line 1176 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1178 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_fscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _fscanf_l(
                                         FILE*       const _Stream,
            char const* const _Format,
                                        _locale_t   const _Locale,
        ...)
    

#line 1188 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfscanf_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1197 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "fscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl fscanf(
                               FILE*       const _Stream,
            char const* const _Format,
        ...)
    

#line 1206 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfscanf_l(_Stream, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1215 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _fscanf_s_l(
                                           FILE*       const _Stream,
            char const* const _Format,
                                          _locale_t   const _Locale,
        ...)
    

#line 1225 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfscanf_s_l(_Stream, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1234 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl fscanf_s(
                                     FILE*       const _Stream,
                char const* const _Format,
            ...)
    

#line 1245 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfscanf_s_l(_Stream, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 1254 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1256 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_scanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _scanf_l(
            char const* const _Format,
                                        _locale_t   const _Locale,
        ...)
    

#line 1265 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfscanf_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1274 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "scanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl scanf(
            char const* const _Format,
        ...)
    

#line 1282 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vfscanf_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1291 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _scanf_s_l(
            char const* const _Format,
                                          _locale_t   const _Locale,
        ...)
    

#line 1300 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vfscanf_s_l((__acrt_iob_func(0)), _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1309 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl scanf_s(
                char const* const _Format,
            ...)
    

#line 1319 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vfscanf_s_l((__acrt_iob_func(0)), _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 1328 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1330 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"



    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Narrow Character Formatted Output Functions (String)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     
     int __cdecl __stdio_common_vsprintf(
                                             unsigned __int64 _Options,
                 char*            _Buffer,
                                             size_t           _BufferCount,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
     int __cdecl __stdio_common_vsprintf_s(
                                             unsigned __int64 _Options,
                     char*            _Buffer,
                                             size_t           _BufferCount,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
     int __cdecl __stdio_common_vsnprintf_s(
                                             unsigned __int64 _Options,
                 char*            _Buffer,
                                             size_t           _BufferCount,
                                             size_t           _MaxCount,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
     int __cdecl __stdio_common_vsprintf_p(
                                             unsigned __int64 _Options,
                     char*            _Buffer,
                                             size_t           _BufferCount,
            char const*      _Format,
                                         _locale_t        _Locale,
                                                va_list          _ArgList
        );

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsnprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _vsnprintf_l(
            char*       const _Buffer,
                                                  size_t      const _BufferCount,
                 char const* const _Format,
                                              _locale_t   const _Locale,
                                                     va_list           _ArgList
        )
    

#line 1391 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf(
            (*__local_stdio_printf_options()) | (1ULL << 0),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1399 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsnprintf(
            char*       const _Buffer,
                                                 size_t      const _BufferCount,
                          char const* const _Format,
                                                    va_list           _ArgList
        )
    

#line 1411 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsnprintf_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1415 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    








#line 1426 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl vsnprintf(
           char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                char const* const _Format,
                                                          va_list           _ArgList
        )
    

#line 1438 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf(
            (*__local_stdio_printf_options()) | (1ULL << 1),
            _Buffer, _BufferCount, _Format, ((void *)0), _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1446 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _vsprintf_l(
           char*       const _Buffer,
                                    char const* const _Format,
                                  _locale_t   const _Locale,
                                         va_list           _ArgList
        )
    

#line 1458 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsnprintf_l(_Buffer, (size_t)-1, _Format, _Locale, _ArgList);
    }
    #line 1462 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "vsprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl vsprintf(
           char*       const _Buffer,
               char const* const _Format,
                                         va_list           _ArgList
        )
    

#line 1473 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsnprintf_l(_Buffer, (size_t)-1, _Format, ((void *)0), _ArgList);
    }
    #line 1477 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsprintf_s_l(
           char*       const _Buffer,
                                                   size_t      const _BufferCount,
                  char const* const _Format,
                                               _locale_t   const _Locale,
                                                      va_list           _ArgList
        )
    

#line 1490 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf_s(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1498 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

         
        
        __inline int __cdecl vsprintf_s(
               char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                char const* const _Format,
                                                          va_list           _ArgList
            )
    

#line 1512 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vsprintf_s_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        }
    #line 1516 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

        
#line 1524 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1526 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsprintf_p_l(
           char*       const _Buffer,
                                                   size_t      const _BufferCount,
                  char const* const _Format,
                                               _locale_t   const _Locale,
                                                      va_list           _ArgList
        )
    

#line 1539 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf_p(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1547 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsprintf_p(
           char*       const _Buffer,
                                                   size_t      const _BufferCount,
                            char const* const _Format,
                                                      va_list           _ArgList
        )
    

#line 1559 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsprintf_p_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1563 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsnprintf_s_l(
           char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                                       size_t      const _MaxCount,
                      char const* const _Format,
                                                   _locale_t   const _Locale,
                                                          va_list          _ArgList
        )
    

#line 1577 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsnprintf_s(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _MaxCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1585 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsnprintf_s(
           char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                                       size_t      const _MaxCount,
                                char const* const _Format,
                                                          va_list           _ArgList
        )
    

#line 1598 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsnprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, ((void *)0), _ArgList);
    }
    #line 1602 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
#line 1611 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

         
        
        __inline int __cdecl vsnprintf_s(
               char*       const _Buffer,
                                                           size_t      const _BufferCount,
                                                           size_t      const _MaxCount,
                                    char const* const _Format,
                                                              va_list           _ArgList
            )
    

#line 1626 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vsnprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, ((void *)0), _ArgList);
        }
    #line 1630 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

        
#line 1639 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1641 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vscprintf_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
                                                va_list           _ArgList
        )
    

#line 1651 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf(
            (*__local_stdio_printf_options()) | (1ULL << 1),
            ((void *)0), 0, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1659 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    __inline int __cdecl _vscprintf(
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 1668 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vscprintf_l(_Format, ((void *)0), _ArgList);
    }
    #line 1672 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vscprintf_p_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
                                                va_list           _ArgList
        )
    

#line 1682 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf_p(
            (*__local_stdio_printf_options()) | (1ULL << 1),
            ((void *)0), 0, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1690 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    __inline int __cdecl _vscprintf_p(
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 1699 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vscprintf_p_l(_Format, ((void *)0), _ArgList);
    }
    #line 1703 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vsnprintf_c_l(
                   char*       const _Buffer,
                                             size_t      const _BufferCount,
            char const* const _Format,
                                         _locale_t   const _Locale,
                                                va_list           _ArgList
        )
    

#line 1715 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int const _Result = __stdio_common_vsprintf(
            (*__local_stdio_printf_options()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        return _Result < 0 ? -1 : _Result;
    }
    #line 1723 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _vsnprintf_c(
          char*       const _Buffer,
                                    size_t      const _BufferCount,
             char const* const _Format,
                                       va_list           _ArgList
        )
    

#line 1735 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsnprintf_c_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
    }
    #line 1739 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_sprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _sprintf_l(
                  char*       const _Buffer,
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 1750 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));

        _Result = _vsprintf_l(_Buffer, _Format, _Locale, _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1761 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl sprintf(
           char*       const _Buffer,
               char const* const _Format,
        ...)
    

#line 1771 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));

        _Result = _vsprintf_l(_Buffer, _Format, ((void *)0), _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1782 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "sprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   int __cdecl sprintf(  char *_Buffer,  char const* _Format, ...); __declspec(deprecated("This function or variable may be unsafe. Consider using " "vsprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   int __cdecl vsprintf(  char *_Buffer,  char const* _Format, va_list _Args);
#line 1789 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _sprintf_s_l(
           char*       const _Buffer,
                                                   size_t      const _BufferCount,
                  char const* const _Format,
                                               _locale_t   const _Locale,
        ...)
    

#line 1801 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsprintf_s_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1810 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

         
        
        __inline int __cdecl sprintf_s(
               char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                char const* const _Format,
            ...)
    

#line 1823 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));
            _Result = _vsprintf_s_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 1832 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 1834 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
#line 1841 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _sprintf_p_l(
           char*       const _Buffer,
                                                   size_t      const _BufferCount,
                  char const* const _Format,
                                               _locale_t   const _Locale,
        ...)
    

#line 1853 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsprintf_p_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1862 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _sprintf_p(
           char*       const _Buffer,
                                                   size_t      const _BufferCount,
                            char const* const _Format,
        ...)
    

#line 1873 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsprintf_p_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1882 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snprintf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _snprintf_l(
            char*       const _Buffer,
                                                  size_t      const _BufferCount,
                 char const* const _Format,
                                              _locale_t   const _Locale,
        ...)
    

#line 1894 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));

        _Result = _vsnprintf_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1905 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    








#line 1916 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl snprintf(
           char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                char const* const _Format,
        ...)
    

#line 1927 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = vsnprintf(_Buffer, _BufferCount, _Format, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1936 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _snprintf(
            char*       const _Buffer,
                                                  size_t      const _BufferCount,
                           char const* const _Format,
        ...)
    

#line 1947 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsnprintf(_Buffer, _BufferCount, _Format, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1956 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))    int __cdecl _snprintf(    char *_Buffer,   size_t _BufferCount,     char const* _Format, ...); __declspec(deprecated("This function or variable may be unsafe. Consider using " "_vsnprintf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))    int __cdecl _vsnprintf(    char *_Buffer,   size_t _BufferCount,     char const* _Format, va_list _Args);
#line 1965 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _snprintf_c_l(
                   char*       const _Buffer,
                                             size_t      const _BufferCount,
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 1977 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsnprintf_c_l(_Buffer, _BufferCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 1986 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _snprintf_c(
          char*       const _Buffer,
                                    size_t      const _BufferCount,
             char const* const _Format,
        ...)
    

#line 1997 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsnprintf_c_l(_Buffer, _BufferCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2006 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _snprintf_s_l(
           char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                                       size_t      const _MaxCount,
                      char const* const _Format,
                                                   _locale_t   const _Locale,
        ...)
    

#line 2019 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsnprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2028 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    
    __inline int __cdecl _snprintf_s(
           char*       const _Buffer,
                                                       size_t      const _BufferCount,
                                                       size_t      const _MaxCount,
                                char const* const _Format,
        ...)
    

#line 2040 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsnprintf_s_l(_Buffer, _BufferCount, _MaxCount, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2049 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
#line 2057 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _scprintf_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 2066 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vscprintf_l(_Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2075 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    __inline int __cdecl _scprintf(
            char const* const _Format,
        ...)
    

#line 2083 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vscprintf_l(_Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2092 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _scprintf_p_l(
            char const* const _Format,
                                         _locale_t   const _Locale,
        ...)
    

#line 2101 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vscprintf_p_l(_Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2110 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     
    __inline int __cdecl _scprintf_p(
            char const* const _Format,
        ...)
    

#line 2118 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vscprintf_p(_Format, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2127 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Narrow Character Formatted Input Functions (String)
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     int __cdecl __stdio_common_vsscanf(
                                            unsigned __int64 _Options,
                  char const*      _Buffer,
                                            size_t           _BufferCount,
            char const*      _Format,
                                        _locale_t        _Locale,
                                               va_list          _ArgList
        );

    
    __inline int __cdecl _vsscanf_l(
                                 char const* const _Buffer,
            char const* const _Format,
                               _locale_t   const _Locale,
                                      va_list           _ArgList
        )
    

#line 2152 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vsscanf(
            (*__local_stdio_scanf_options ()),
            _Buffer, (size_t)-1, _Format, _Locale, _ArgList);
    }
    #line 2158 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl vsscanf(
                                 char const* const _Buffer,
            char const* const _Format,
                                      va_list           _ArgList
        )
    

#line 2168 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return _vsscanf_l(_Buffer, _Format, ((void *)0), _ArgList);
    }
    #line 2172 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _vsscanf_s_l(
                                 char const* const _Buffer,
            char const* const _Format,
                               _locale_t   const _Locale,
                                      va_list           _ArgList
        )
    

#line 2183 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        return __stdio_common_vsscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Buffer, (size_t)-1, _Format, _Locale, _ArgList);
    }
    #line 2189 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        #pragma warning(push)
        #pragma warning(disable: 6530) 

        
        __inline int __cdecl vsscanf_s(
                                     char const* const _Buffer,
                char const* const _Format,
                                          va_list           _ArgList
            )
    

#line 2204 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            return _vsscanf_s_l(_Buffer, _Format, ((void *)0), _ArgList);
        }
    #line 2208 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

        
#line 2215 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

        #pragma warning(pop)

    #line 2219 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_sscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _sscanf_l(
                                          char const* const _Buffer,
            char const* const _Format,
                                        _locale_t   const _Locale,
        ...)
    

#line 2229 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsscanf_l(_Buffer, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2238 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

      __declspec(deprecated("This function or variable may be unsafe. Consider using " "sscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl sscanf(
                                char const* const _Buffer,
            char const* const _Format,
        ...)
    

#line 2247 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));
        _Result = _vsscanf_l(_Buffer, _Format, ((void *)0), _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2256 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _sscanf_s_l(
                                            char const* const _Buffer,
            char const* const _Format,
                                          _locale_t   const _Locale,
        ...)
    

#line 2266 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));
        _Result = _vsscanf_s_l(_Buffer, _Format, _Locale, _ArgList);
        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2275 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    

        
        __inline int __cdecl sscanf_s(
                                      char const* const _Buffer,
                char const* const _Format,
            ...)
    

#line 2286 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
        {
            int _Result;
            va_list _ArgList;
            ((void)(__va_start(&_ArgList, _Format)));

            _Result = vsscanf_s(_Buffer, _Format, _ArgList);

            ((void)(_ArgList = (va_list)0));
            return _Result;
        }
    #line 2297 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #line 2299 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #pragma warning(push)
    #pragma warning(disable: 6530) 

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snscanf_s_l" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _snscanf_l(
            char const* const _Buffer,
                                            size_t      const _BufferCount,
            char const* const _Format,
                                        _locale_t   const _Locale,
        ...)
    

#line 2313 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));

        _Result = __stdio_common_vsscanf(
            (*__local_stdio_scanf_options ()),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2326 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

     __declspec(deprecated("This function or variable may be unsafe. Consider using " "_snscanf_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
    __inline int __cdecl _snscanf(
            char const* const _Buffer,
                                            size_t      const _BufferCount,
                      char const* const _Format,
        ...)
    

#line 2336 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));

        _Result = __stdio_common_vsscanf(
            (*__local_stdio_scanf_options ()),
            _Buffer, _BufferCount, _Format, ((void *)0), _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2349 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"


    
    __inline int __cdecl _snscanf_s_l(
              char const* const _Buffer,
                                              size_t      const _BufferCount,
            char const* const _Format,
                                          _locale_t   const _Locale,
        ...)
    

#line 2361 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Locale)));

        _Result = __stdio_common_vsscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Buffer, _BufferCount, _Format, _Locale, _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2374 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    
    __inline int __cdecl _snscanf_s(
            char const* const _Buffer,
                                            size_t      const _BufferCount,
                    char const* const _Format,
        ...)
    

#line 2384 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
    {
        int _Result;
        va_list _ArgList;
        ((void)(__va_start(&_ArgList, _Format)));

        _Result = __stdio_common_vsscanf(
            (*__local_stdio_scanf_options ()) | (1ULL << 0),
            _Buffer, _BufferCount, _Format, ((void *)0), _ArgList);

        ((void)(_ArgList = (va_list)0));
        return _Result;
    }
    #line 2397 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

    #pragma warning(pop)

    

#line 2403 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"



    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Non-ANSI Names for Compatibility
    //
    //-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    

        

        


#line 2419 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

        __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_tempnam" ". See online help for details."))
         char* __cdecl tempnam(
              char const* _Directory,
              char const* _FilePrefix
            );

        

#line 2429 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"

         __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_fcloseall" ". See online help for details."))  int   __cdecl fcloseall(void);
              __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_fdopen" ". See online help for details."))     FILE* __cdecl fdopen(  int _FileHandle,   char const* _Format);
         __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_fgetchar" ". See online help for details."))   int   __cdecl fgetchar(void);
              __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_fileno" ". See online help for details."))     int   __cdecl fileno(  FILE* _Stream);
         __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_flushall" ". See online help for details."))   int   __cdecl flushall(void);
         __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_fputchar" ". See online help for details."))   int   __cdecl fputchar(  int _Ch);
              __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_getw" ". See online help for details."))       int   __cdecl getw(  FILE* _Stream);
         __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_putw" ". See online help for details."))       int   __cdecl putw(  int _Ch,   FILE* _Stream);
              __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_rmtmp" ". See online help for details."))      int   __cdecl rmtmp(void);

    #line 2441 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
#line 2442 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 2449 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\stdio.h"
#line 19 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\stdint.h"
//
// stdint.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <stdint.h> header.
//
#pragma once






#pragma warning(push)
#pragma warning(disable:   4514 4820 )

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int_least8_t;
typedef short              int_least16_t;
typedef int                int_least32_t;
typedef long long          int_least64_t;
typedef unsigned char      uint_least8_t;
typedef unsigned short     uint_least16_t;
typedef unsigned int       uint_least32_t;
typedef unsigned long long uint_least64_t;

typedef signed char        int_fast8_t;
typedef int                int_fast16_t;
typedef int                int_fast32_t;
typedef long long          int_fast64_t;
typedef unsigned char      uint_fast8_t;
typedef unsigned int       uint_fast16_t;
typedef unsigned int       uint_fast32_t;
typedef unsigned long long uint_fast64_t;

typedef long long          intmax_t;
typedef unsigned long long uintmax_t;

// These macros must exactly match those in the Windows SDK's intsafe.h.








































    
    
    




#line 97 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\stdint.h"







































#pragma warning(pop) 

#line 139 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\stdint.h"
#line 20 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
#line 1 "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207\\include\\stdarg.h"
//
// stdarg.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <stdarg.h> header.
//
#pragma once




#pragma warning(push)
#pragma warning(disable:   4514 4820 )

__pragma(pack(push, 8))










__pragma(pack(pop))

#pragma warning(pop) 
#line 21 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"
//
// time.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// The C Standard Library <time.h> header.
//
#pragma once




#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"
//
// corecrt_wtime.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// This file declares the wide character (wchar_t) time functionality, shared
// by <time.h> and <wchar.h>.
//
#pragma once



#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Types
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
struct tm
{
    int tm_sec;   // seconds after the minute - [0, 60] including leap second
    int tm_min;   // minutes after the hour - [0, 59]
    int tm_hour;  // hours since midnight - [0, 23]
    int tm_mday;  // day of the month - [1, 31]
    int tm_mon;   // months since January - [0, 11]
    int tm_year;  // years since 1900
    int tm_wday;  // days since Sunday - [0, 6]
    int tm_yday;  // days since January 1 - [0, 365]
    int tm_isdst; // daylight savings time flag
};


//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Wide String Time Functions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wasctime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 
 
 wchar_t* __cdecl _wasctime(
      struct tm const* _Tm
    );

 

 errno_t __cdecl _wasctime_s(
        wchar_t*         _Buffer,
                                          size_t           _SizeInWords,
                                                       struct tm const* _Tm
    );


#line 66 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"

 

 size_t __cdecl wcsftime(
       wchar_t*         _Buffer,
                               size_t           _SizeInWords,
                             wchar_t const*   _Format,
                               struct tm const* _Tm
    );

 

 size_t __cdecl _wcsftime_l(
       wchar_t*         _Buffer,
                               size_t           _SizeInWords,
                             wchar_t const*   _Format,
                               struct tm const* _Tm,
                           _locale_t        _Locale
    );

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wctime32_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 wchar_t* __cdecl _wctime32(
      __time32_t const* _Time
    );


 errno_t __cdecl _wctime32_s(
        wchar_t*          _Buffer,
                                      size_t            _SizeInWords,
                                                       __time32_t const* _Time
    );


#line 104 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"

 
 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_wctime64_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 wchar_t* __cdecl _wctime64(
      __time64_t const* _Time
    );


 errno_t __cdecl _wctime64_s(
        wchar_t*          _Buffer,
                                      size_t            _SizeInWords,
                                                       __time64_t const* _Time);


#line 123 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"


 errno_t __cdecl _wstrdate_s(
       wchar_t* _Buffer,
                                                                                size_t   _SizeInWords
    );


#line 134 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wstrdate_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   wchar_t* __cdecl _wstrdate( wchar_t *_Buffer);
#line 139 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"


 errno_t __cdecl _wstrtime_s(
       wchar_t* _Buffer,
                                                                                size_t   _SizeInWords
    );


#line 150 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_wstrtime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   wchar_t* __cdecl _wstrtime( wchar_t *_Buffer);
#line 155 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Inline Definitions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    




















         
        static __inline wchar_t * __cdecl _wctime(
              time_t const* const _Time)
        {
            return _wctime64(_Time);
        }

        
        static __inline errno_t __cdecl _wctime_s(
                  wchar_t*      const _Buffer,
                                                             size_t        const _SizeInWords,
                                                             time_t const* const _Time
            )
        {
            return _wctime64_s(_Buffer, _SizeInWords, _Time);
        }

    #line 203 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"
#line 204 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\corecrt_wtime.h"

__pragma(pack(pop))

#pragma warning(pop) 
#line 14 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )


__pragma(pack(push, 8))



#line 24 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Types
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
typedef long clock_t;

struct _timespec32
{
    __time32_t tv_sec;
    long       tv_nsec;
};

struct _timespec64
{
    __time64_t tv_sec;
    long       tv_nsec;
};


    struct timespec
    {
        time_t tv_sec;  // Seconds - >= 0
        long   tv_nsec; // Nanoseconds - [0, 999999999]
    };
#line 51 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"



// The number of clock ticks per second






//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Time Zone and Daylight Savings Time Data and Accessors
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// Nonzero if Daylight Savings Time is used
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_daylight" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 int* __cdecl __daylight(void);



// Offset for Daylight Savings Time
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_dstbias" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 long* __cdecl __dstbias(void);



// Difference in seconds between GMT and local time
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_timezone" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 long* __cdecl __timezone(void);



// Standard and Daylight Savings Time time zone names
    __declspec(deprecated("This function or variable may be unsafe. Consider using " "_get_tzname" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char** __cdecl __tzname(void);



  
 errno_t __cdecl _get_daylight(
      int* _Daylight
    );

 
 errno_t __cdecl _get_dstbias(
      long* _DaylightSavingsBias
    );

  
 errno_t __cdecl _get_timezone(
      long* _TimeZone
    );

 
 errno_t __cdecl _get_tzname(
                             size_t* _ReturnValue,
      char*   _Buffer,
                              size_t  _SizeInBytes,
                              int     _Index
    );



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// AppCRT Time Functions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 
 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "asctime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl asctime(
      struct tm const* _Tm
    );


     
    
     errno_t __cdecl asctime_s(
            char*            _Buffer,
                                            size_t           _SizeInBytes,
                                                         struct tm const* _Tm
        );
#line 136 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"


#line 142 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

 
 clock_t __cdecl clock(void);

 
 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ctime32_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _ctime32(
      __time32_t const* _Time
    );


 errno_t __cdecl _ctime32_s(
        char*             _Buffer,
                                        size_t            _SizeInBytes,
                                                     __time32_t const* _Time
    );


#line 165 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

 
 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_ctime64_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 char* __cdecl _ctime64(
      __time64_t const* _Time
    );


 errno_t __cdecl _ctime64_s(
        char*             _Buffer,
                                          size_t            _SizeInBytes,
                                                       __time64_t const* _Time
    );


#line 185 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

 
 double __cdecl _difftime32(
      __time32_t _Time1,
      __time32_t _Time2
    );

 
 double __cdecl _difftime64(
      __time64_t _Time1,
      __time64_t _Time2
    );

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_gmtime32_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 struct tm* __cdecl _gmtime32(
      __time32_t const* _Time
    );


 errno_t __cdecl _gmtime32_s(
      struct tm*        _Tm,
       __time32_t const* _Time
    );

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_gmtime64_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 struct tm* __cdecl _gmtime64(
      __time64_t const* _Time
    );


 errno_t __cdecl _gmtime64_s(
      struct tm*        _Tm,
       __time64_t const* _Time
    );

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_localtime32_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 struct tm* __cdecl _localtime32(
      __time32_t const* _Time
    );


 errno_t __cdecl _localtime32_s(
      struct tm*        _Tm,
       __time32_t const* _Time
    );

 
  __declspec(deprecated("This function or variable may be unsafe. Consider using " "_localtime64_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
 struct tm* __cdecl _localtime64(
      __time64_t const* _Time
    );


 errno_t __cdecl _localtime64_s(
      struct tm*        _Tm,
       __time64_t const* _Time
    );

 
 __time32_t __cdecl _mkgmtime32(
      struct tm* _Tm
    );

 
 __time64_t __cdecl _mkgmtime64(
      struct tm* _Tm
    );


 __time32_t __cdecl _mktime32(
      struct tm* _Tm
    );


 __time64_t __cdecl _mktime64(
      struct tm* _Tm
    );

 

 size_t __cdecl strftime(
       char*            _Buffer,
                               size_t           _SizeInBytes,
        char const*      _Format,
                               struct tm const* _Tm
    );

 

 size_t __cdecl _strftime_l(
           char*            _Buffer,
                               size_t           _MaxSize,
        char const*      _Format,
                               struct tm const* _Tm,
                           _locale_t        _Locale
    );


 errno_t __cdecl _strdate_s(
       char*  _Buffer,
                                                                              size_t _SizeInBytes
    );


#line 295 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strdate_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))   char* __cdecl _strdate( char *_Buffer);
#line 300 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"


 errno_t __cdecl _strtime_s(
       char*  _Buffer,
                                                                              size_t _SizeInBytes
    );


#line 311 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

__declspec(deprecated("This function or variable may be unsafe. Consider using " "_strtime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))  char* __cdecl _strtime( char *_Buffer);
#line 316 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

 __time32_t __cdecl _time32(
      __time32_t* _Time
    );

 __time64_t __cdecl _time64(
      __time64_t* _Time
    );

 
 
 int __cdecl _timespec32_get(
      struct _timespec32* _Ts,
       int                 _Base
    );

 
 
 int __cdecl _timespec64_get(
      struct _timespec64* _Ts,
       int                 _Base
    );



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// DesktopCRT Time Functions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


     void __cdecl _tzset(void);

    // The Win32 API GetLocalTime and SetLocalTime should be used instead.
    __declspec(deprecated("This function or variable has been superceded by newer library " "or operating system functionality. Consider using " "GetLocalTime" " " "instead. See online help for details."))
     unsigned __cdecl _getsystime(
          struct tm* _Tm
        );

    __declspec(deprecated("This function or variable has been superceded by newer library " "or operating system functionality. Consider using " "SetLocalTime" " " "instead. See online help for details."))
     unsigned __cdecl _setsystime(
          struct tm* _Tm,
          unsigned   _Milliseconds
        );

#line 363 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Inline Function Definitions
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


    






























































































































          __declspec(deprecated("This function or variable may be unsafe. Consider using " "ctime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
        static __inline char* __cdecl ctime(
              time_t const* const _Time
            )
        {
            return _ctime64(_Time);
        }

         
        static __inline double __cdecl difftime(
              time_t const _Time1,
              time_t const _Time2
            )
        {
            return _difftime64(_Time1, _Time2);
        }

          __declspec(deprecated("This function or variable may be unsafe. Consider using " "gmtime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
        static __inline struct tm* __cdecl gmtime(
              time_t const* const _Time)
        {
            return _gmtime64(_Time);
        }

        __declspec(deprecated("This function or variable may be unsafe. Consider using " "localtime_s" " instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS. " "See online help for details."))
        static __inline struct tm* __cdecl localtime(
              time_t const* const _Time
            )
        {
            return _localtime64(_Time);
        }

         
        static __inline time_t __cdecl _mkgmtime(
              struct tm* const _Tm
            )
        {
            return _mkgmtime64(_Tm);
        }

        
        static __inline time_t __cdecl mktime(
              struct tm* const _Tm
            )
        {
            return _mktime64(_Tm);
        }

        static __inline time_t __cdecl time(
              time_t* const _Time
            )
        {
            return _time64(_Time);
        }

         
        static __inline int __cdecl timespec_get(
              struct timespec* const _Ts,
               int              const _Base
            )
        {
            return _timespec64_get((struct _timespec64*)_Ts, _Base);
        }

        
            
            static __inline errno_t __cdecl ctime_s(
                    char*         const _Buffer,
                                                    size_t        const _SizeInBytes,
                                                                 time_t const* const _Time
                )
            {
                return _ctime64_s(_Buffer, _SizeInBytes, _Time);
            }

        

























#line 602 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"
            
            static __inline errno_t __cdecl gmtime_s(
                      struct tm*    const _Tm,
                       time_t const* const _Time
                )
            {
                return _gmtime64_s(_Tm, _Time);
            }

            
                static __inline errno_t __cdecl localtime_s(
                      struct tm*    const _Tm,
                       time_t const* const _Time
                )
            {
                return _localtime64_s(_Tm, _Time);
            }
        #line 620 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"
        #line 621 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

    #line 623 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

#line 625 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Non-ANSI Names for Compatibility
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


    

    
        __declspec(deprecated("The POSIX name for this item is deprecated. Instead, use the ISO C " "and C++ conformant name: " "_tzset" ". See online help for details."))  void __cdecl tzset(void);
    #line 640 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"

#line 642 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"



__pragma(pack(pop))

#pragma warning(pop) 
#line 649 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\time.h"
#line 22 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

#line 1 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\fcntl.h"
//
// fcntl.h
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// File control options used by _open().
//
#pragma once





#pragma warning(push)
#pragma warning(disable: 4324  4514 4574 4710 4793 4820 4995 4996 28719 28726 28727 )











// O_TEXT files have <cr><lf> sequences translated to <lf> on read()'s and <lf>
// sequences translated to <cr><lf> on write()'s







// macro to translate the C 2.0 name used to force binary mode for files












    
    
    
    
    
    
    
    
    
    
    
    
    
    
#line 64 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\fcntl.h"


#pragma warning(pop) 

#line 69 "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt\\fcntl.h"
#line 24 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

/* 鈹€鈹€ Platform detection 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */






#line 33 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"


#line 36 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"



#line 40 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

#line 42 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"



#line 46 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

#line 48 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"



#line 52 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

#line 54 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

/* MSVC compiler detection */


#pragma warning(disable: 4996)  
#pragma warning(disable: 4244)  
#pragma warning(disable: 4267)  
#pragma warning(disable: 4100)  
#pragma warning(disable: 4127)  

/* 鈹€鈹€ MSVC POSIX compatibility macros 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

/* __attribute__ not supported */


/* POSIX strdup 鈫?_strdup */


/* POSIX strtok_r 鈫?strtok_s (same signature) */


/* POSIX strcasecmp/strncasecmp 鈫?_stricmp/_strnicmp */



/* POSIX snprintf emulation for MSVC < 2015 (VC14 / _MSC_VER 1900) */


#line 83 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

/* POSIX localtime_r/gmtime_r wrappers */
static __inline struct tm *localtime_r(const time_t *t, struct tm *result)
{
    if (localtime_s(result, t) != 0) return ((void *)0);
    return result;
}
static __inline struct tm *gmtime_r(const time_t *t, struct tm *result)
{
    if (gmtime_s(result, t) != 0) return ((void *)0);
    return result;
}



#line 99 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

/* 鈹€鈹€ Cross-platform inline keyword 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */







#line 109 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

/* 鈹€鈹€ DLL Export / Import (for lr_js.h consumer) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */
/* NOTE: lr_platform.h is internal; lr_js.h handles public API exports.
   This section is only for internal use. */





#line 119 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"

/* 鈹€鈹€ Windows-specific headers 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */




#line 126 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"


#line 129 "F:\\GitHub\\LR_JS\\src\\lr_platform.h"
