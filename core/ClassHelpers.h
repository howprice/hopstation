#pragma once

//
// Use inside class definition to make the class non-instantiable i.e. all members must be static.
// Static classes simplify the codebase.
// n.b. All deleted class member functions should be public. See https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rc-delete
// 
#define NON_INSTANTIABLE_STATIC_CLASS(class_name) class_name() = delete;\
	class_name(const class_name&) = delete;\
	class_name(const class_name&&) = delete

// Use inside class definition to make non-copyable
// Typically, macros are generally constructed so they require a semi-colon at the end of the line, just like normal statements.
// https://stackoverflow.com/questions/28770213/macro-to-make-class-noncopyable
#define NON_COPYABLE_CLASS(class_name) class_name(const class_name&) = delete;\
	class_name& operator=(const class_name&) = delete
