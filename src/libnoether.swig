/* libnoether.swig — SWIG interface for the Noether ER-301 Object. */

%module noether_libnoether

%include <od/glue/mod.cpp.swig>

%{
#undef SWIGLUA
#include <Noether.h>
#include <NoetherDisplay.h>
#include <NoetherReel.h>
#include <NoetherScale.h>
#define SWIGLUA
using namespace noether;
%}

%include <Noether.h>
%include <NoetherDisplay.h>
%include <NoetherReel.h>
%include <NoetherScale.h>
