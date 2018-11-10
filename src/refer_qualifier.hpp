/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#ifndef SCOPES_REFER_QUALIFIER_HPP
#define SCOPES_REFER_QUALIFIER_HPP

#include "pointer_type.hpp"
#include "qualify_type.hpp"

namespace scopes {

//------------------------------------------------------------------------------
// REFER QUALIFIER
//------------------------------------------------------------------------------

struct ReferQualifier : Qualifier {
    enum { Kind = QK_Refer };

    static bool classof(const Qualifier *T);

    ReferQualifier(uint64_t _flags, Symbol _storage_class);

    void stream_prefix(StyledStream &ss) const;
    void stream_postfix(StyledStream &ss) const;

    uint64_t flags;
    Symbol storage_class;
};

const Type *refer_type(const Type *element_type, uint64_t flags,
    Symbol storage_class);

} // namespace scopes

#endif // SCOPES_REFER_QUALIFIER_HPP
