# Shared warning profile. Apply with astral_set_warnings(<target>).
option(ASTRAL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(astral_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive-
      /w14242 # conversion from int to short, possible loss of data
      /w14254 # operator: conversion from int to char, possible loss of data
      /w14263 # member function does not override any base class virtual member
      /w14265 # class has virtual functions, but destructor is not virtual
      /w14287 # unsigned/negative constant mismatch
      /w14296 # expression is always false
      /w14311 # pointer truncation from void* to u32
      /w14545 # expression before comma evaluates to a function
      /w14546 # function call before comma missing argument list
      /w14547 # operator before comma has no effect
      /w14549 # operator before comma has no effect
      /w14555 # expression has no effect
      /w14640 # volatile member is not thread-safe
      /w14826 # conversion is sign-extended
      /w14905 # wide string literal cast to char*
      /w14906 # string literal cast to char*
      /w14928 # illegal copy-initialization
    )
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wcast-qual
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
    )
  endif()

  if(ASTRAL_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
