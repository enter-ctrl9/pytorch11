#include "ATen/core/interned_strings_class.h"

// This file is compiled with -O0 because the fully-macro-expanded
// function is huge and only called once at startup.

#if defined(__clang__)
#pragma clang optimize off
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("O0")
#endif

namespace c10 {
InternedStrings::InternedStrings()
    : sym_to_info_(static_cast<size_t>(_keys::num_symbols)) {
#define REGISTER_SYMBOL(n, s)        \
  string_to_sym_[#n "::" #s] = n::s; \
  sym_to_info_[n::s] = {namespaces::n, #n "::" #s, #s};

  FORALL_NS_SYMBOLS(REGISTER_SYMBOL)
#undef REGISTER_SYMBOL
}
} // namespace c10

#if defined(__clang__)
#pragma clang optimize on
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
