//===-- runtime/derived-type.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_RUNTIME_DERIVED_TYPE_H_
#define FORTRAN_RUNTIME_DERIVED_TYPE_H_

// Data structures for use in static type information tables that define derived
// type specializations, suitable for residence in read-only storage.
// These type descriptions are used for type equivalence tests, inquiry
// intrinsic functions, TYPE IS/CLASS IS, ALLOCATE and DEALLOCATE, finalization.
// assignment, NAMELIST formatted I/O, type-bound procedure calls, &c.
//
// Pointers in these classes are non-owning because their instances are
// intended to be generated by the f18 compiler and effectively static
// const during Fortran program execution.  The constructors here are
// for testing purposes.

#include "type-code.h"
#include "flang/ISO_Fortran_binding.h"
#include <cinttypes>
#include <cstddef>

namespace Fortran::runtime {

class Descriptor;

using TypeParameterValue = ISO::CFI_index_t;

class TypeParameter {
public:
  // KIND type parameter
  TypeParameter(const char *name, TypeCode type, TypeParameterValue value)
      : name_{name}, typeCode_{type}, value_{value} {}

  // LEN type parameter
  TypeParameter(
      const char *name, TypeCode type, TypeParameterValue value, int which)
      : name_{name}, typeCode_{type}, which_{which}, value_{value} {}

  const char *name() const { return name_; }
  const TypeCode typeCode() const { return typeCode_; }

  bool IsKindTypeParameter() const { return which_ < 0; }
  bool IsLenTypeParameter() const { return which_ >= 0; }

  // Returns the static value of a KIND type parameter, or the default
  // value of a LEN type parameter.
  TypeParameterValue StaticValue() const { return value_; }

  // Returns the static value of a KIND type parameter, or an
  // instantiated value of LEN type parameter.
  TypeParameterValue GetValue(const Descriptor &) const;

private:
  const char *name_;
  TypeCode typeCode_; // INTEGER, but not necessarily default kind
  int which_{-1}; // index into DescriptorAddendum LEN type parameter values
  TypeParameterValue value_; // default in the case of LEN type parameter
};

// Components that have any need for a descriptor will either reference
// a static descriptor that applies to all instances, or will *be* a
// descriptor.  Be advised: the base addresses in static descriptors
// are null.  Most runtime interfaces separate the data address from that
// of the descriptor, and ignore the encapsulated base address in the
// descriptor.  Some interfaces, e.g. calls to interoperable procedures,
// cannot pass a separate data address, and any static descriptor being used
// in that kind of situation must be copied and customized.
// Static descriptors are flagged in their attributes.
class Component {
public:
  // TODO: constructor

  const char *name() const { return name_; }
  TypeCode typeCode() const { return typeCode_; }
  const Descriptor *staticDescriptor() const { return staticDescriptor_; }

  bool IsParent() const { return (flags_ & PARENT) != 0; }
  bool IsPrivate() const { return (flags_ & PRIVATE) != 0; }
  bool IsDescriptor() const { return (flags_ & IS_DESCRIPTOR) != 0; }

  template <typename A> A &Locate(char *dtInstance) const {
    return *reinterpret_cast<A *>(dtInstance + offset_);
  }
  template <typename A> const A &Locate(const char *dtInstance) const {
    return *reinterpret_cast<const A *>(dtInstance + offset_);
  }

private:
  enum Flag { PARENT = 1, PRIVATE = 2, IS_DESCRIPTOR = 4 };

  const char *name_{nullptr};
  std::uint32_t flags_{0};
  TypeCode typeCode_{CFI_type_other};
  const Descriptor *staticDescriptor_{nullptr};
  std::uint64_t offset_{0}; // byte offset into derived type instance
};

struct ExecutableCode {
  ExecutableCode() {}
  ExecutableCode(const ExecutableCode &) = default;
  ExecutableCode &operator=(const ExecutableCode &) = default;
  std::intptr_t host{0};
  std::intptr_t device{0};
};

struct TypeBoundProcedure {
  enum Flag {
    INITIALIZER = 1,
    ELEMENTAL = 2,
    ASSIGNMENT = 4,
    ASSUMED_RANK_FINAL = 8
  };
  bool IsFinalForRank(int rank) const {
    return (flags & ASSUMED_RANK_FINAL) || ((finalRank >> rank) & 1);
  }
  std::uint32_t flags{0};
  std::uint32_t finalRank{0}; // LE bit n -> is FINAL subroutine for rank n
  const char *name;
  ExecutableCode code;
};

// Represents derived type with no KIND type parameters or a complete
// specialization of a derived type; i.e., all KIND type parameters
// have values here.
// Extended derived types have the EXTENDS flag set and place their base
// component first in the component descriptions, which is significant for
// the execution of FINAL subroutines.
class DerivedType {
public:
  DerivedType(const char *name, int kindParams, int lenParams,
      const TypeParameter *, int components, const Component *, int tbps,
      const TypeBoundProcedure *, const char *init, std::size_t instanceBytes);

  const char *name() const { return name_; }

  int kindParameters() const { return kindParameters_; }
  int lenParameters() const { return lenParameters_; }

  // KIND type parameter values are stored here in the DerivedType
  // description; LEN type parameter values are stored in the
  // addendum of a descriptor of an instance of the type.
  const TypeParameter &kindTypeParameter(int n) const {
    return typeParameter_[n];
  }
  const TypeParameter &lenTypeParameter(int n) const {
    return typeParameter_[kindParameters_ + n];
  }

  int components() const { return components_; }
  const Component &component(int n) const { return component_[n]; }

  int typeBoundProcedures() const { return typeBoundProcedures_; }
  const TypeBoundProcedure &typeBoundProcedure(int n) const {
    return typeBoundProcedure_[n];
  }

  DerivedType &set_sequence() {
    flags_ |= SEQUENCE;
    return *this;
  }
  DerivedType &set_bind_c() {
    flags_ |= BIND_C;
    return *this;
  }

  std::size_t SizeInBytes() const { return bytes_; }
  bool IsExtension() const {
    return components_ > 0 && component_[0].IsParent();
  }
  bool AnyPrivate() const;
  bool IsSequence() const { return (flags_ & SEQUENCE) != 0; }
  bool IsBindC() const { return (flags_ & BIND_C) != 0; }
  bool IsInitializable() const {
    return initializer_ || initTBP_ >= 0 ||
        (flags_ & (INIT_ZERO | INIT_COMPONENT)) != 0;
  }
  bool IsInitZero() const { return (flags_ & INIT_ZERO) != 0; }
  bool IsFinalizable() const { return (flags_ & FINALIZABLE) != 0; }

  // SAME_TYPE_AS() and EXTENDS_TYPE() intrinsic inquiry functions,
  // and a predicate for TYPE IS and CLASS IS testing in SELECT TYPE.
  bool SameTypeAs(const DerivedType &) const; // ignores type parameters
  bool Extends(const DerivedType &) const;
  bool TypeIs(const DerivedType &) const; // compares KIND type parameters

  // Subtle: If a derived type has an adjustable component (a hidden
  // allocatable whose type &/or bounds depend on LEN type parameters),
  // the compiler must generate an initialization subroutine for the
  // type, and Initialize() will call it.
  void Initialize(char *) const;

  void DestroyNonParentComponents(char *, bool finalize = true) const;
  void DestroyScalarInstance(char *, bool finalize = true) const;

private:
  enum Flag {
    SEQUENCE = 1,
    BIND_C = 2,
    FINALIZABLE = 4,
    INIT_ZERO = 8,
    INIT_COMPONENT = 16
  };

  const char *name_{""}; // NUL-terminated constant text
  int kindParameters_{0};
  int lenParameters_{0}; // values are in descriptors
  const TypeParameter *typeParameter_{nullptr}; // array; KIND first
  int components_{0}; // *not* including type parameters
  const Component *component_{nullptr}; // array; parent first
  int typeBoundProcedures_{0};
  const TypeBoundProcedure *typeBoundProcedure_{nullptr}; // array
  std::uint64_t flags_{0};
  const char *initializer_{nullptr};
  std::size_t bytes_{0};
  int initTBP_{-1};
};
} // namespace Fortran::runtime
#endif // FORTRAN_RUNTIME_DERIVED_TYPE_H_
