//===-- runtime/derived-type.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "derived-type.h"
#include "descriptor.h"
#include "terminator.h"
#include <cstring>

namespace Fortran::runtime {

TypeParameterValue TypeParameter::GetValue(const Descriptor &descriptor) const {
  if (which_ < 0) {
    return value_;
  } else {
    return descriptor.Addendum()->LenParameterValue(which_);
  }
}

DerivedType::DerivedType(const char *n, int kps, int lps,
    const TypeParameter *tp, int cs, const Component *ca, int tbps,
    const TypeBoundProcedure *tbp, const char *init, std::size_t sz)
    : name_{n}, kindParameters_{kps}, lenParameters_{lps}, typeParameter_{tp},
      components_{cs}, component_{ca}, typeBoundProcedures_{tbps},
      typeBoundProcedure_{tbp}, initializer_{init}, bytes_{sz} {
  for (int j{0}; j < typeBoundProcedures_; ++j) {
    if (tbp[j].flags & TypeBoundProcedure::INITIALIZER) {
      initTBP_ = j;
    }
    if (tbp[j].finalRank ||
        (tbp[j].flags & TypeBoundProcedure::ASSUMED_RANK_FINAL)) {
      flags_ |= FINALIZABLE;
    }
  }
  if (!initializer_ && initTBP_ < 0) {
    for (int j{0}; j < components_; ++j) {
      if (component_[j].IsDescriptor()) {
        flags_ |= INIT_ZERO;
      } else if (const Descriptor *
          staticDesc{component_[j].staticDescriptor()}) {
        if (const DescriptorAddendum * addendum{staticDesc->Addendum()}) {
          if (const DerivedType * type{addendum->derivedType()}) {
            if (type->IsInitializable() && !type->IsInitZero()) {
              flags_ |= INIT_COMPONENT;
            }
          }
        }
      }
    }
  }
}

void DerivedType::Initialize(char *instance) const {
  if (initializer_) {
    std::memcpy(instance, initializer_, bytes_);
    return;
  }
  if (initTBP_ >= 0) {
    if (auto f{reinterpret_cast<void (*)(char *)>(
            typeBoundProcedure_[initTBP_].code.host)}) {
      f(instance);
      return;
    }
  }
  if (flags_ & INIT_ZERO) {
    std::memset(instance, 0, bytes_);
  }
  if (flags_ & INIT_COMPONENT) {
    for (int j{0}; j < components_; ++j) {
      if (const Descriptor * staticDesc{component_[j].staticDescriptor()}) {
        staticDesc->Initialize(&component_[j].Locate<char>(instance));
      }
    }
  }
}

void DerivedType::DestroyNonParentComponents(
    char *instance, bool finalize) const {
  for (int j{0}; j < components_; ++j) {
    const Component &comp{component_[j]};
    if (comp.IsParent()) {
      continue;
    }
    if (comp.IsDescriptor()) {
      comp.Locate<Descriptor>(instance).Deallocate(finalize);
    } else if (const Descriptor * staticDescriptor{comp.staticDescriptor()}) {
      staticDescriptor->Destroy(&comp.Locate<char>(instance), finalize);
    }
  }
}

void DerivedType::DestroyScalarInstance(char *instance, bool finalize) const {
  if (finalize && IsFinalizable()) {
    for (int j{0}; j < typeBoundProcedures_; ++j) {
      const auto &tbp{typeBoundProcedure_[j]};
      if ((tbp.flags & TypeBoundProcedure::ELEMENTAL) && (tbp.finalRank & 1)) {
        if (auto f{reinterpret_cast<void (*)(char *)>(tbp.code.host)}) {
          f(instance);
        }
      }
    }
  }
  DestroyNonParentComponents(instance, finalize);
  if (IsExtension()) {
    const Descriptor *staticParentDescriptor{component_[0].staticDescriptor()};
    Terminator terminator{__FILE__, __LINE__};
    RUNTIME_CHECK(terminator, staticParentDescriptor);
    const auto *parentAddendum{staticParentDescriptor->Addendum()};
    RUNTIME_CHECK(terminator, parentAddendum);
    const auto *parentType{parentAddendum->derivedType()};
    RUNTIME_CHECK(terminator, parentType);
    parentType->DestroyScalarInstance(instance, finalize);
  }
}
} // namespace Fortran::runtime
