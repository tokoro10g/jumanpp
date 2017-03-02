//
// Created by Arseny Tolmachev on 2017/02/28.
//

#ifndef JUMANPP_FEATURE_IMPL_H
#define JUMANPP_FEATURE_IMPL_H

#include "core/analysis/extra_nodes.h"
#include "core/core_types.h"
#include "core/dictionary.h"
#include "core/impl/feature_types.h"
#include "core/impl/field_reader.h"
#include "util/array_slice.h"
#include "util/status.hpp"
#include "util/stl_util.h"
#include "util/string_piece.h"
#include "util/types.hpp"

namespace jumanpp {
namespace core {
namespace features {
namespace impl {

class DicListTraversal {
  dic::impl::IntListTraversal trav_;

 public:
  DicListTraversal(const dic::impl::IntListTraversal& trav) : trav_{trav} {}
  bool next(i32* result) { return trav_.readOneCumulative(result); }
};

/**
 * This showsh which dictionary storage field for a column
 * should be used for computations
 */
enum class LengthFieldSource { Invalid, Strings, Positions };

class PrimitiveFeatureContext {
  analysis::ExtraNodesContext* extraCtx;
  dic::FieldsHolder* fields;

 public:
  DicListTraversal traversal(i32 fieldIdx, i32 fieldPtr) const {
    auto& fld = fields->at(fieldIdx);
    auto trav = fld.postions.listAt(fieldPtr);
    return DicListTraversal{trav};
  }

  i32 providedFeature(EntryPtr entryPtr, u32 index) const {
    auto node = extraCtx->node(entryPtr);
    if (node == nullptr ||
        node->header.type != analysis::ExtraNodeType::Unknown) {
      return 0;
    }
    auto features = node->header.unk.providedValues;
    return features[index];
  }

  Status checkFieldType(
      i32 field, std::initializer_list<spec::ColumnType> columnTypes) const;

  Status checkProvidedFeature(u32 index) const { return Status::Ok(); }

  i32 lengthOf(i32 fieldNum, i32 fieldPtr, LengthFieldSource field) {
    if (fieldPtr < 0) {
      return extraCtx->lengthOf(fieldNum, fieldPtr);
    }
    auto fld = fields->at(fieldNum);
    switch (field) {
      case LengthFieldSource::Positions:
        return fld.postions.lengthOf(fieldPtr);
      case LengthFieldSource::Strings:
        return fld.strings.lengthOf(fieldPtr);
      default:
        return -1;
    }
  }

  Status setLengthField(i32 fieldNum, LengthFieldSource* field) {
    auto fld = fields->at(fieldNum);
    auto type = fld.columnType;
    if (type == spec::ColumnType::StringList) {
      *field = LengthFieldSource::Positions;
    } else if (type == spec::ColumnType::String) {
      *field = LengthFieldSource::Strings;
    } else {
      return Status::InvalidState()
             << "field " << fld.name << " typed " << fld.columnType
             << " can not be used for length calculation";
    }

    return Status::Ok();
  }
};

class FeatureImplBase {
 public:
  constexpr FeatureImplBase() {}
  virtual ~FeatureImplBase() = default;
};

class PrimitiveFeatureImpl : public FeatureImplBase {
 public:
  virtual Status initialize(PrimitiveFeatureContext* ctx,
                            const PrimitiveFeature& f) = 0;

  virtual void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                     const util::ArraySlice<i32>& entry,
                     util::MutableArraySlice<u64>* features) const noexcept = 0;
};

template <typename Impl>
class DynamicPrimitiveFeature : public PrimitiveFeatureImpl {
  Impl impl;

 public:
  virtual Status initialize(PrimitiveFeatureContext* ctx,
                            const PrimitiveFeature& f) override {
    return impl.initialize(ctx, f);
  }

  virtual void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                     const util::ArraySlice<i32>& entry,
                     util::MutableArraySlice<u64>* features) const
      noexcept override {
    impl.apply(ctx, entryPtr, entry, features);
  }
};

class CopyPrimFeatureImpl {
  u32 fieldIdx;
  u32 featureIdx;

 public:
  CopyPrimFeatureImpl() {}
  constexpr CopyPrimFeatureImpl(u32 fieldIdx, u32 featureIdx)
      : fieldIdx{fieldIdx}, featureIdx{featureIdx} {}

  Status initialize(PrimitiveFeatureContext* ctx, const PrimitiveFeature& f);

  inline void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                    const util::ArraySlice<i32>& entry,
                    util::MutableArraySlice<u64>* features) const noexcept {
    features->at(featureIdx) = entry.at(fieldIdx);
  }
};

class ProvidedPrimFeatureImpl {
  u32 providedIdx;
  u32 featureIdx;

 public:
  ProvidedPrimFeatureImpl() {}
  constexpr ProvidedPrimFeatureImpl(u32 providedIdx, u32 featureIdx)
      : providedIdx(providedIdx), featureIdx(featureIdx) {}

  Status initialize(PrimitiveFeatureContext* ctx, const PrimitiveFeature& f);

  inline void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                    const util::ArraySlice<i32>& entry,
                    util::MutableArraySlice<u64>* features) const noexcept {
    features->at(featureIdx) = ctx->providedFeature(entryPtr, providedIdx);
  }
};

class LengthPrimFeatureImpl {
  u32 fieldIdx;
  u32 featureIdx;
  LengthFieldSource field = LengthFieldSource::Invalid;

 public:
  LengthPrimFeatureImpl() {}
  constexpr LengthPrimFeatureImpl(u32 fieldIdx, u32 featureIdx,
                                  LengthFieldSource fld)
      : fieldIdx{fieldIdx}, featureIdx{featureIdx}, field{fld} {}

  Status initialize(PrimitiveFeatureContext* ctx, const PrimitiveFeature& f);

  inline void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                    const util::ArraySlice<i32>& entry,
                    util::MutableArraySlice<u64>* features) const noexcept {
    auto fldPtr = entry[fieldIdx];
    auto length = ctx->lengthOf(fieldIdx, fldPtr, field);
    JPP_DCHECK_NE(length, -1);
    features->at(featureIdx) = length;
  }
};

class MatchDicPrimFeatureImpl {
  u32 fieldIdx;
  u32 featureIdx;
  util::ArraySlice<i32> matchData;

 public:
  MatchDicPrimFeatureImpl() {}
  template <size_t S>
  constexpr MatchDicPrimFeatureImpl(u32 fieldIdx, u32 featureIdx,
                                    const i32 (&matchData)[S])
      : fieldIdx{fieldIdx}, featureIdx{featureIdx}, matchData{matchData} {}

  Status initialize(PrimitiveFeatureContext* ctx, const PrimitiveFeature& f);

  inline void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                    const util::ArraySlice<i32>& entry,
                    util::MutableArraySlice<u64>* features) const noexcept {
    auto elem = entry.at(fieldIdx);
    i32 result = 0;
    if (contains(matchData, elem)) {
      result = 1;
    }
    features->at(featureIdx) = result;
  }
};

class MatchAnyDicPrimFeatureImpl {
  u32 fieldIdx;
  u32 featureIdx;
  util::ArraySlice<i32> matchData;

 public:
  MatchAnyDicPrimFeatureImpl() {}
  template <size_t S>
  constexpr MatchAnyDicPrimFeatureImpl(u32 fieldIdx, u32 featureIdx,
                                       const i32 (&matchData)[S])
      : fieldIdx{fieldIdx}, featureIdx{featureIdx}, matchData{matchData} {}

  Status initialize(PrimitiveFeatureContext* ctx, const PrimitiveFeature& f);

  inline void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
                    const util::ArraySlice<i32>& entry,
                    util::MutableArraySlice<u64>* features) const noexcept {
    auto elem = entry.at(fieldIdx);
    auto trav = ctx->traversal(fieldIdx, elem);
    i32 result = 0;
    i32 value = -1;
    while (trav.next(&value)) {
      if (contains(matchData, value)) {
        result = 1;
        break;
      }
    }
    features->at(featureIdx) = result;
  }
};

class PrimitiveFeaturesDynamicHolder {
  std::vector<std::unique_ptr<PrimitiveFeatureImpl>> features_;

 public:
  Status initialize(PrimitiveFeatureContext* ctx,
                    util::ArraySlice<PrimitiveFeature> featureData);

  void apply(PrimitiveFeatureContext* ctx, EntryPtr entryPtr,
             const util::ArraySlice<i32>& entry,
             util::MutableArraySlice<u64>* features) const noexcept;
};

}  // impl
}  // features
}  // core
}  // jumanpp

#endif  // JUMANPP_FEATURE_IMPL_H