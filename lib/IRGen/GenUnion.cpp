//===--- GenUnion.cpp - Swift IR Generation For 'union' Types -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for algebraic data types (ADTs,
//  or 'union' types) in Swift.  This includes creating the IR type as
//  well as emitting the basic access operations.
//
//  The current scheme is that all such types with are represented
//  with an initial word indicating the variant, followed by a union
//  of all the possibilities.  This is obviously completely acceptable
//  to everyone and will not benefit from further refinement.
//
//  As a completely unimportant premature optimization, we do emit
//  types with only a single variant as simple structs wrapping that
//  variant.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/Basic/Optional.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"

#include "LoadableTypeInfo.h"
#include "GenMeta.h"
#include "GenProto.h"
#include "GenType.h"
#include "GenUnion.h"
#include "IRGenDebugInfo.h"
#include "IRGenModule.h"
#include "ScalarTypeInfo.h"

using namespace swift;
using namespace irgen;

namespace {
  class UnionImplStrategy;
  
  /// An abstract base class for TypeInfo implementations of union types.
  // FIXME: not always loadable or even fixed-size!
  class UnionTypeInfo : public LoadableTypeInfo {
  public:
    // FIXME: Derive spare bits from element layout.
    UnionTypeInfo(llvm::StructType *T, Size S, llvm::BitVector SB,
                  Alignment A, IsPOD_t isPOD)
      : LoadableTypeInfo(T, S, std::move(SB), A, isPOD) {}

    llvm::StructType *getStorageType() const {
      return cast<llvm::StructType>(TypeInfo::getStorageType());
    }

    bool isIndirectArgument(ExplosionKind kind) const override {
      // FIXME!
      return false;
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address dest) const override {
      // FIXME!
      initialize(IGF, params, dest);
    }

    /// Emit the construction sequence for a union case.
    virtual void emitInjection(IRGenFunction &IGF,
                               UnionElementDecl *elt,
                               Explosion &params,
                               Explosion &out) const = 0;
    
    /// Emit a branch on the case contained in a union value.
    virtual void emitSwitch(IRGenFunction &IGF,
                            Explosion &value,
                            ArrayRef<std::pair<UnionElementDecl*,
                                               llvm::BasicBlock*>> dests,
                            llvm::BasicBlock *defaultDest) const = 0;
    
    /// Project a case value out of a union value. This does not check that the
    /// union actually contains a value of the given case.
    virtual void emitProject(IRGenFunction &IGF,
                             Explosion &inUnion,
                             UnionElementDecl *theCase,
                             Explosion &out) const = 0;
    
    /// Given an incomplete UnionTypeInfo, completes layout of the storage type
    /// and calculates its size and alignment.
    virtual void layoutUnionType(TypeConverter &TC, UnionDecl *theUnion,
                                 UnionImplStrategy &strategy) = 0;
  };
  
  /// Abstract base class for unions with one or cases that carry payloads.
  class PayloadUnionTypeInfoBase : public UnionTypeInfo {
  protected:
    // The number of extra tag bits outside of the payload required to
    // discriminate union cases.
    unsigned ExtraTagBitCount = 0;
    // The number of possible values for the extra tag bits that are used.
    // Log2(NumExtraTagValues - 1) + 1 == ExtraTagBitCount
    unsigned NumExtraTagValues = 0;
    
  public:
    PayloadUnionTypeInfoBase(llvm::StructType *T, Size S, Alignment A,
                             IsPOD_t isPOD)
      : UnionTypeInfo(T, S, {}, A, isPOD)
    {}
    
    virtual Size getExtraTagBitOffset() const = 0;

    void getSchema(ExplosionSchema &schema) const {
      schema.add(ExplosionSchema::Element::forScalar(
                                         getStorageType()->getElementType(0)));
      if (ExtraTagBitCount > 0)
        schema.add(ExplosionSchema::Element::forScalar(
                                         getStorageType()->getElementType(1)));
    }
    
    unsigned getExplosionSize(ExplosionKind kind) const {
      return ExtraTagBitCount > 0 ? 2 : 1;
    }
    
    Address projectPayload(IRGenFunction &IGF, Address addr) const {
      return IGF.Builder.CreateStructGEP(addr, 0, Size(0));
    }
    
    Address projectExtraTagBits(IRGenFunction &IGF, Address addr) const {
      assert(ExtraTagBitCount > 0 && "does not have extra tag bits");
      return IGF.Builder.CreateStructGEP(addr, 1, getExtraTagBitOffset());
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {
      e.add(IGF.Builder.CreateLoad(projectPayload(IGF, addr)));
      if (ExtraTagBitCount > 0)
        e.add(IGF.Builder.CreateLoad(projectExtraTagBits(IGF, addr)));
    }
    
    void loadAsCopy(IRGenFunction &IGF, Address addr, Explosion &e) const {
      Explosion tmp(e.getKind());
      loadAsTake(IGF, addr, tmp);
      copy(IGF, tmp, e);
    }
    
    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {
      Explosion old(e.getKind());
      if (!isPOD(ResilienceScope::Local))
        loadAsTake(IGF, addr, old);
      initialize(IGF, e, addr);
      if (!isPOD(ResilienceScope::Local))
        consume(IGF, old);
    }
    
    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src) const {
      Explosion srcVal(ExplosionKind::Minimal);
      loadAsCopy(IGF, src, srcVal);
      assign(IGF, srcVal, dest);
    }
    
    void assignWithTake(IRGenFunction &IGF, Address dest, Address src) const {
      Explosion srcVal(ExplosionKind::Minimal);
      loadAsTake(IGF, src, srcVal);
      assign(IGF, srcVal, dest);
    }
    
    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {
      IGF.Builder.CreateStore(e.claimNext(), projectPayload(IGF, addr));
      if (ExtraTagBitCount > 0)
        IGF.Builder.CreateStore(e.claimNext(), projectExtraTagBits(IGF, addr));
    }
    
    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      dest.add(src.claim(getExplosionSize(ExplosionKind::Minimal)));
    }
    
    void destroy(IRGenFunction &IGF, Address addr) const {
      if (isPOD(ResilienceScope::Local))
        return;

      Explosion val(ExplosionKind::Minimal);
      loadAsTake(IGF, addr, val);
      consume(IGF, val);
    }
  };

  /// A UnionTypeInfo implementation which has a single case with a payload and
  /// one or more additional no-payload cases.
  class SinglePayloadUnionTypeInfo : public PayloadUnionTypeInfoBase {
    UnionElementDecl *PayloadElement = nullptr;
    // FIXME - non-fixed payloads
    const FixedTypeInfo *PayloadTypeInfo = nullptr;
  public:
    /// FIXME: Spare bits from the payload not exhausted by the extra
    /// inhabitants we used.
    SinglePayloadUnionTypeInfo(llvm::StructType *T, Size S, Alignment A,
                              IsPOD_t isPOD)
      : PayloadUnionTypeInfoBase(T, S, A, isPOD) {}
    
    Size getExtraTagBitOffset() const override {
      return PayloadTypeInfo->getFixedSize();
    }
    
    llvm::Value *packUnionPayload(IRGenFunction &IGF, Explosion &src,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      assert(isComplete());
      
      PackUnionPayload pack(IGF, bitWidth);
      // Pack payload.
      pack.addAtOffset(src.claimNext(), offset);
      
      // Pack tag bits, if any.
      if (ExtraTagBitCount > 0) {
        unsigned extraTagOffset
          = PayloadTypeInfo->getFixedSize().getValueInBits() + offset;
        
        pack.addAtOffset(src.claimNext(), extraTagOffset);
      }
      
      return pack.get();
    }
    
    void unpackUnionPayload(IRGenFunction &IGF, llvm::Value *outerPayload,
                            Explosion &dest,
                            unsigned offset) const override {
      assert(isComplete());
      
      UnpackUnionPayload unpack(IGF, outerPayload);
      
      // Unpack our inner payload.
      dest.add(unpack.claimAtOffset(getStorageType()->getElementType(0),
                                    offset));
      
      // Unpack our extra tag bits, if any.
      if (ExtraTagBitCount > 0) {
        unsigned extraTagOffset
          = PayloadTypeInfo->getFixedSize().getValueInBits() + offset;
        
        dest.add(unpack.claimAtOffset(getStorageType()->getElementType(1),
                                      extraTagOffset));
      }
    }
    
    void emitSwitch(IRGenFunction &IGF,
                    Explosion &value,
                    ArrayRef<std::pair<UnionElementDecl*,
                                       llvm::BasicBlock*>> dests,
                    llvm::BasicBlock *defaultDest) const override {
      auto &C = IGF.IGM.getLLVMContext();
      
      // Create a map of the destination blocks for quicker lookup.
      llvm::DenseMap<UnionElementDecl*,llvm::BasicBlock*> destMap(dests.begin(),
                                                                  dests.end());
      // Create an unreachable branch for unreachable switch defaults.
      auto *unreachableBB = llvm::BasicBlock::Create(C);

      // If there was no default branch in SIL, use the unreachable branch as
      // the default.
      if (!defaultDest)
        defaultDest = unreachableBB;
      
      auto blockForCase = [&](UnionElementDecl *theCase) -> llvm::BasicBlock* {
        auto found = destMap.find(theCase);
        if (found == destMap.end())
          return defaultDest;
        else
          return found->second;
      };
      
      llvm::Value *payload = value.claimNext();
      llvm::BasicBlock *payloadDest = blockForCase(PayloadElement);
      unsigned extraInhabitantCount
        = PayloadTypeInfo->getFixedExtraInhabitantCount();
      
      // If there are extra tag bits, switch over them first.
      SmallVector<llvm::BasicBlock*, 2> tagBitBlocks;
      if (ExtraTagBitCount > 0) {
        llvm::Value *tagBits = value.claimNext();
        
        auto *swi = IGF.Builder.CreateSwitch(tagBits, unreachableBB,
                                             NumExtraTagValues);
        
        // If we have extra inhabitants, we need to check for them in the
        // zero-tag case. Otherwise, we switch directly to the payload case.
        if (extraInhabitantCount > 0) {
          auto bb = llvm::BasicBlock::Create(C);
          tagBitBlocks.push_back(bb);
          swi->addCase(llvm::ConstantInt::get(C,APInt(ExtraTagBitCount,0)), bb);
        } else {
          tagBitBlocks.push_back(payloadDest);
          swi->addCase(llvm::ConstantInt::get(C,APInt(ExtraTagBitCount,0)),
                       payloadDest);
        }
        
        for (unsigned i = 1; i < NumExtraTagValues; ++i) {
          auto bb = llvm::BasicBlock::Create(C);
          tagBitBlocks.push_back(bb);
          swi->addCase(llvm::ConstantInt::get(C,APInt(ExtraTagBitCount,i)), bb);
        }
        
        // Continue by emitting the extra inhabitant dispatch, if any.
        if (extraInhabitantCount > 0)
          IGF.Builder.emitBlock(tagBitBlocks[0]);
      }
      
      auto elements = PayloadElement->getParentUnion()->getAllElements();
      auto elti = elements.begin(), eltEnd = elements.end();
      if (*elti == PayloadElement)
        ++elti;
      
      // Advance the union element iterator, skipping the payload case.
      auto nextCase = [&]() -> UnionElementDecl* {
        assert(elti != eltEnd);
        auto result = *elti;
        ++elti;
        if (elti != eltEnd && *elti == PayloadElement)
          ++elti;
        return result;
      };
      
      // If there are no extra tag bits, or they're set to zero, then we either
      // have a payload, or an empty case represented using an extra inhabitant.
      // Check the extra inhabitant cases if we have any.
      unsigned payloadBits = PayloadTypeInfo->getFixedSize().getValueInBits();
      if (extraInhabitantCount > 0) {
        auto *swi = IGF.Builder.CreateSwitch(payload, payloadDest);
        for (unsigned i = 0; i < extraInhabitantCount && elti != eltEnd; ++i) {
          auto v = PayloadTypeInfo->getFixedExtraInhabitantValue(IGF.IGM,
                                                               payloadBits, i);
          swi->addCase(v, blockForCase(nextCase()));
        }
      }
      
      // We should have handled the payload case either in extra inhabitant
      // or in extra tag dispatch by now.
      assert(IGF.Builder.hasPostTerminatorIP() &&
             "did not handle payload case");
      
      // Handle the cases covered by each tag bit value.
      unsigned casesPerTag = 1 << ExtraTagBitCount;
      for (unsigned i = 1, e = tagBitBlocks.size(); i < e; ++i) {
        assert(elti != eltEnd &&
               "ran out of cases before running out of extra tags?");
        IGF.Builder.emitBlock(tagBitBlocks[i]);
        auto swi = IGF.Builder.CreateSwitch(payload, unreachableBB);
        for (unsigned tag = 0; tag < casesPerTag && elti != eltEnd; ++tag) {
          auto v = llvm::ConstantInt::get(C, APInt(payloadBits, tag));
          swi->addCase(v, blockForCase(nextCase()));
        }
      }
      
      // Delete the unreachable default block if we didn't use it, or emit it
      // if we did.
      if (unreachableBB->use_empty()) {
        delete unreachableBB;
      } else {
        IGF.Builder.emitBlock(unreachableBB);
        IGF.Builder.CreateUnreachable();
      }
    }
    
    void emitProject(IRGenFunction &IGF,
                     Explosion &inUnion,
                     UnionElementDecl *theCase,
                     Explosion &out) const override {
      // Only the payload case has anything to project. The other cases are
      // empty.
      if (theCase != PayloadElement) {
        inUnion.claimAll();
        return;
      }
        
      llvm::Value *payload = inUnion.claimNext();
      if (ExtraTagBitCount > 0)
        inUnion.claimNext();
      // FIXME non-loadable payloads
      cast<LoadableTypeInfo>(PayloadTypeInfo)
        ->unpackUnionPayload(IGF, payload, out, 0);
    }
    
  private:
    // Get the index of a union element among the non-payload cases.
    unsigned getSimpleElementTagIndex(UnionElementDecl *elt) const {
      assert(elt != PayloadElement && "is payload element");
      unsigned i = 0;
      // FIXME: linear search
      for (auto *unionElt : elt->getParentUnion()->getAllElements()) {
        if (elt == unionElt)
          return i;
        if (unionElt != PayloadElement)
          ++i;
      }
      llvm_unreachable("element was not a member of union");
    }
    
    void addExtraTagBitValue(IRGenFunction &IGF,
                             Explosion &out, unsigned value) const {
      if (ExtraTagBitCount > 0) {
        llvm::Value *zeroTagBits
          = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                   APInt(ExtraTagBitCount, value));
        out.add(zeroTagBits);
        return;
      }
      assert(value == 0 && "setting non-zero extra tag value with no tag bits");
    }
    
  public:
    void emitInjection(IRGenFunction &IGF,
                       UnionElementDecl *elt,
                       Explosion &params,
                       Explosion &out) const {
      // The payload case gets its native representation. If there are extra
      // tag bits, set them to zero.
      unsigned payloadSize = PayloadTypeInfo->getFixedSize().getValueInBits();
      
      if (elt == PayloadElement) {
        // FIXME
        auto &loadablePayloadTI = cast<LoadableTypeInfo>(*PayloadTypeInfo);
        llvm::Value *payload
          = loadablePayloadTI.packUnionPayload(IGF, params, payloadSize, 0);
        out.add(payload);
        
        addExtraTagBitValue(IGF, out, 0);
        return;
      }
      
      // Non-payload cases use extra inhabitants, if any, or are discriminated
      // by setting the tag bits.
      unsigned tagIndex = getSimpleElementTagIndex(elt);
      unsigned numExtraInhabitants
        = PayloadTypeInfo->getFixedExtraInhabitantCount();
      llvm::Value *payload;
      unsigned extraTagValue;
      if (tagIndex < numExtraInhabitants) {
        payload = PayloadTypeInfo->getFixedExtraInhabitantValue(IGF.IGM,
                                                                payloadSize,
                                                                tagIndex);
        extraTagValue = 0;
      } else {
        tagIndex -= numExtraInhabitants;
        
        // Factor the extra tag value from the payload value.
        unsigned payloadValue;
        if (payloadSize >= 32) {
          payloadValue = tagIndex;
          extraTagValue = 1U;
        } else {
          payloadValue = tagIndex & ((1U << payloadSize) - 1U);
          extraTagValue = (tagIndex >> payloadSize) + 1U;
        }
        
        payload = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                         APInt(payloadSize, payloadValue));
      }

      out.add(payload);
      addExtraTagBitValue(IGF, out, extraTagValue);
    }
    
  private:
    std::tuple<llvm::BasicBlock *, llvm::Value*, llvm::Value*>
    testUnionContainsPayload(IRGenFunction &IGF, Explosion &src) const {
      auto *endBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());

      llvm::Value *payload = src.claimNext();
      llvm::Value *extraBits = nullptr;

      // We only need to apply the payload operation if the union contains a
      // value of the payload case.
      
      // If we have extra tag bits, they will be zero if we contain a payload.
      if (ExtraTagBitCount > 0) {
        extraBits = src.claimNext();
        llvm::Value *zero = llvm::ConstantInt::get(extraBits->getType(), 0);
        llvm::Value *isZero = IGF.Builder.CreateICmp(llvm::CmpInst::ICMP_EQ,
                                                     extraBits, zero);
        
        auto *trueBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        IGF.Builder.CreateCondBr(isZero, trueBB, endBB);
        
        IGF.Builder.emitBlock(trueBB);
      }
      
      // If we used extra inhabitants to represent empty case discriminators,
      // weed them out.
      unsigned numExtraInhabitants
        = PayloadTypeInfo->getFixedExtraInhabitantCount();
      if (numExtraInhabitants > 0) {
        auto *payloadBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        auto *swi = IGF.Builder.CreateSwitch(payload, payloadBB);
        
        auto elements = PayloadElement->getParentUnion()->getAllElements();
        unsigned inhabitant = 0;
        for (auto i = elements.begin(), end = elements.end();
             i != end && inhabitant < numExtraInhabitants;
             ++i, ++inhabitant) {
          if (*i == PayloadElement)
            ++i;
          auto xi = PayloadTypeInfo->getFixedExtraInhabitantValue(IGF.IGM,
                              PayloadTypeInfo->getFixedSize().getValueInBits(),
                              inhabitant);
          swi->addCase(xi, endBB);
        }
        
        IGF.Builder.emitBlock(payloadBB);
      }
      
      return {endBB, payload, extraBits};
    }

  public:
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      if (isPOD(ResilienceScope::Local)) {
        reexplode(IGF, src, dest);
        return;
      }
      
      // Copy the payload, if we have it.
      llvm::BasicBlock *endBB;
      llvm::Value *payload, *extraTag;
      std::tie(endBB, payload, extraTag) = testUnionContainsPayload(IGF, src);

      Explosion payloadValue(ExplosionKind::Minimal);
      Explosion payloadCopy(ExplosionKind::Minimal);
      auto &loadableTI = cast<LoadableTypeInfo>(*PayloadTypeInfo);
      loadableTI.unpackUnionPayload(IGF, payload, payloadValue, 0);
      loadableTI.copy(IGF, payloadValue, payloadCopy);
      payloadCopy.claimAll();
      
      IGF.Builder.CreateBr(endBB);
      IGF.Builder.emitBlock(endBB);
      
      // Copy to the new explosion.
      dest.add(payload);
      if (extraTag) dest.add(extraTag);
    }
    
    void consume(IRGenFunction &IGF, Explosion &src) const {
      if (isPOD(ResilienceScope::Local)) {
        src.claimAll();
        return;
      }

      // Check that we have a payload.
      llvm::BasicBlock *endBB;
      llvm::Value *payload, *extraTag;
      std::tie(endBB, payload, extraTag) = testUnionContainsPayload(IGF, src);

      // If we did, consume it.
      Explosion payloadValue(ExplosionKind::Minimal);
      auto &loadableTI = cast<LoadableTypeInfo>(*PayloadTypeInfo);
      loadableTI.unpackUnionPayload(IGF, payload, payloadValue, 0);
      loadableTI.consume(IGF, payloadValue);

      IGF.Builder.CreateBr(endBB);
      IGF.Builder.emitBlock(endBB);
    }
    
    void layoutUnionType(TypeConverter &TC, UnionDecl *theUnion,
                         UnionImplStrategy &strategy) override;
  };

  /// A UnionTypeInfo implementation which has multiple payloads.
  class MultiPayloadUnionTypeInfo : public PayloadUnionTypeInfoBase {
    // The spare bits shared by all payloads, if any.
    // Invariant: The size of the bit
    // vector is the size of the payload in bits, rounded up to a byte boundary.
    llvm::BitVector CommonSpareBits;
    
    // The cases with payloads, in tag order.
    // FIXME: non-fixed type info
    std::vector<std::pair<UnionElementDecl*, const FixedTypeInfo*>>
      PayloadCases;
    
    // The cases without payloads, in discriminator order.
    std::vector<UnionElementDecl *> NoPayloadCases;
    
    // The number of tag values used for no-payload cases.
    unsigned NumEmptyElementTags;
  public:
    /// FIXME: Spare bits common to aggregate elements and not used for tags.
    MultiPayloadUnionTypeInfo(llvm::StructType *T, Size S, Alignment A,
                           IsPOD_t isPOD)
      : PayloadUnionTypeInfoBase(T, S, A, isPOD) {}

    Size getExtraTagBitOffset() const override {
      return Size(CommonSpareBits.size()+7U/8U);
    }
    
  private:
    /// Gather spare bits into the low bits of a smaller integer value.
    llvm::Value *gatherSpareBits(IRGenFunction &IGF,
                                 llvm::Value *spareBits,
                                 unsigned resultBitWidth) const {
      auto destTy
        = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), resultBitWidth);
      unsigned usedBits = 0;
      llvm::Value *result = nullptr;
      
      for (int i = CommonSpareBits.find_first(); i != -1;
           i = CommonSpareBits.find_next(i)) {
        assert(i >= 0);
        unsigned u = i;
        assert(u >= usedBits && "used more bits than we've processed?!");
        
        // Shift the bits into place.
        llvm::Value *newBits;
        if (u > 0)
          newBits = IGF.Builder.CreateLShr(spareBits, u - usedBits);
        else
          newBits = spareBits;
        newBits = IGF.Builder.CreateTrunc(newBits, destTy);
        
        // See how many consecutive bits we have.
        unsigned numBits = 1;
        ++u;
        for (unsigned e = CommonSpareBits.size();
             u < e && CommonSpareBits[u]; ++u)
          ++numBits;
        
        // Mask out the selected bits.
        auto val = APInt::getAllOnesValue(numBits);
        if (numBits < resultBitWidth)
          val = val.zext(resultBitWidth);
        val = val.shl(usedBits);
        auto *mask = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), val);
        newBits = IGF.Builder.CreateAnd(newBits, mask);
        
        // Accumulate the result.
        if (result)
          result = IGF.Builder.CreateOr(result, newBits);
        else
          result = newBits;
        
        usedBits += numBits;
        i = u;
      }
      
      return result;
    }
    
    /// The number of empty cases representable by each tag value.
    /// Equal to the size of the payload minus the spare bits used for tags.
    unsigned getNumCaseBits() const {
      return CommonSpareBits.size() - CommonSpareBits.count();
    }
    
    unsigned getNumCasesPerTag() const {
      unsigned numCaseBits = getNumCaseBits();
      return numCaseBits >= 32
        ? 0x80000000 : 1 << numCaseBits;
    }
    
    /// Extract the payload-discriminating tag from a payload and optional
    /// extra tag value.
    llvm::Value *extractPayloadTag(IRGenFunction &IGF,
                                   llvm::Value *payload,
                                   llvm::Value *extraTagBits) const {
      unsigned numSpareBits = CommonSpareBits.count();
      llvm::Value *tag = nullptr;
      unsigned numTagBits = numSpareBits + ExtraTagBitCount;
      
      // Get the tag bits from spare bits, if any.
      if (numSpareBits > 0) {
        tag = gatherSpareBits(IGF, payload, numTagBits);
      }
      
      // Get the extra tag bits, if any.
      if (ExtraTagBitCount > 0) {
        assert(extraTagBits);
        if (!tag) {
          return extraTagBits;
        } else {
          extraTagBits = IGF.Builder.CreateZExt(extraTagBits, tag->getType());
          extraTagBits = IGF.Builder.CreateShl(extraTagBits,
                                               numTagBits - ExtraTagBitCount);
          return IGF.Builder.CreateOr(tag, extraTagBits);
        }
      }
      assert(!extraTagBits);
      return tag;
    }

  public:
    void emitSwitch(IRGenFunction &IGF,
                    Explosion &value,
                    ArrayRef<std::pair<UnionElementDecl*,
                                       llvm::BasicBlock*>> dests,
                    llvm::BasicBlock *defaultDest) const override {
      auto &C = IGF.IGM.getLLVMContext();

      // Create a map of the destination blocks for quicker lookup.
      llvm::DenseMap<UnionElementDecl*,llvm::BasicBlock*> destMap(dests.begin(),
                                                                  dests.end());
      
      // Create an unreachable branch for unreachable switch defaults.
      auto *unreachableBB = llvm::BasicBlock::Create(C);
      
      // If there was no default branch in SIL, use the unreachable branch as
      // the default.
      if (!defaultDest)
        defaultDest = unreachableBB;
      
      auto blockForCase = [&](UnionElementDecl *theCase) -> llvm::BasicBlock* {
        auto found = destMap.find(theCase);
        if (found == destMap.end())
          return defaultDest;
        else
          return found->second;
      };

      llvm::Value *payload = value.claimNext();
      llvm::Value *extraTagBits = nullptr;
      if (ExtraTagBitCount > 0)
        extraTagBits = value.claimNext();

      // Extract and switch on the tag bits.
      llvm::Value *tag = extractPayloadTag(IGF, payload, extraTagBits);
      unsigned numTagBits
        = cast<llvm::IntegerType>(tag->getType())->getBitWidth();
      
      auto *tagSwitch = IGF.Builder.CreateSwitch(tag, unreachableBB,
                                       PayloadCases.size() + NumEmptyElementTags);
      
      // Switch over the tag bits for payload cases.
      unsigned tagIndex = 0;
      for (auto &payloadCasePair : PayloadCases) {
        UnionElementDecl *payloadCase = payloadCasePair.first;
        tagSwitch->addCase(llvm::ConstantInt::get(C,APInt(numTagBits,tagIndex)),
                           blockForCase(payloadCase));
        ++tagIndex;
      }
      
      // Switch over the no-payload cases.
      unsigned casesPerTag = getNumCasesPerTag();
      
      auto elti = NoPayloadCases.begin(), eltEnd = NoPayloadCases.end();
      
      for (unsigned i = 0; i < NumEmptyElementTags; ++i) {
        assert(elti != eltEnd &&
               "ran out of cases before running out of extra tags?");
        auto *tagBB = llvm::BasicBlock::Create(C);
        tagSwitch->addCase(llvm::ConstantInt::get(C,APInt(numTagBits,tagIndex)),
                           tagBB);
        
        // Switch over the cases for this tag.
        IGF.Builder.emitBlock(tagBB);
        auto *caseSwitch = IGF.Builder.CreateSwitch(payload, unreachableBB);
        for (unsigned idx = 0; idx < casesPerTag && elti != eltEnd; ++idx) {
          auto v = interleaveSpareBits(IGF.IGM, CommonSpareBits,
                                       CommonSpareBits.size(),
                                       tagIndex, idx);
          caseSwitch->addCase(v, blockForCase(*elti));
          ++elti;
        }

        ++tagIndex;
      }
      
      // Delete the unreachable default block if we didn't use it, or emit it
      // if we did.
      if (unreachableBB->use_empty()) {
        delete unreachableBB;
      } else {
        IGF.Builder.emitBlock(unreachableBB);
        IGF.Builder.CreateUnreachable();
      }
    }
    
  private:
    static APInt getAPIntFromBitVector(const llvm::BitVector &bits) {
      SmallVector<llvm::integerPart, 2> parts;
      
      for (unsigned i = 0; i < bits.size();) {
        llvm::integerPart part = 0UL;
        for (llvm::integerPart bit = 1; bit != 0 && i < bits.size();
             ++i, bit <<= 1) {
          if (bits[i])
            part |= bit;
        }
        parts.push_back(part);
      }
      
      return APInt(bits.size(), parts);
    }

    void projectPayload(IRGenFunction &IGF,
                        llvm::Value *payload,
                        unsigned payloadTag,
                        const LoadableTypeInfo &payloadTI,
                        Explosion &out) const {
      // If we have spare bits, we have to mask out any set tag bits packed
      // there.
      if (CommonSpareBits.any()) {
        unsigned spareBitCount = CommonSpareBits.count();
        if (spareBitCount < 32)
          payloadTag &= (1U << spareBitCount) - 1U;
        if (payloadTag != 0) {
          APInt mask = ~getAPIntFromBitVector(CommonSpareBits);
          auto maskVal = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                                mask);
          payload = IGF.Builder.CreateAnd(payload, maskVal);
        }
      }
      
      // Unpack the payload.
      payloadTI.unpackUnionPayload(IGF, payload, out, 0);
    }
    
  public:
    void emitProject(IRGenFunction &IGF,
                     Explosion &inValue,
                     UnionElementDecl *theCase,
                     Explosion &out) const override {
      auto foundPayload = std::find_if(PayloadCases.begin(), PayloadCases.end(),
        [&](const std::pair<UnionElementDecl*, const FixedTypeInfo*> &e) {
          return e.first == theCase;
        });
      
      // Non-payload cases project to an empty explosion.
      if (foundPayload == PayloadCases.end()) {
        inValue.claimAll();
        return;
      }
      
      llvm::Value *payload = inValue.claimNext();
      // We don't need the tag bits.
      if (ExtraTagBitCount > 0)
        inValue.claimNext();
      
      // Unpack the payload.
      projectPayload(IGF, payload, foundPayload - PayloadCases.begin(),
                     cast<LoadableTypeInfo>(*foundPayload->second), out);
    }
    
    llvm::Value *packUnionPayload(IRGenFunction &IGF, Explosion &src,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      assert(isComplete());
      
      PackUnionPayload pack(IGF, bitWidth);
      // Pack the payload.
      pack.addAtOffset(src.claimNext(), offset);
      // Pack the extra bits, if any.
      if (ExtraTagBitCount > 0) {
        pack.addAtOffset(src.claimNext(), CommonSpareBits.size() + offset);
      }
      return pack.get();
    }
    
    void unpackUnionPayload(IRGenFunction &IGF, llvm::Value *outerPayload,
                            Explosion &dest,
                            unsigned offset) const override {
      assert(isComplete());

      UnpackUnionPayload unpack(IGF, outerPayload);
      // Unpack the payload.
      dest.add(unpack.claimAtOffset(getStorageType()->getElementType(0),
                                    offset));
      // Unpack the extra bits, if any.
      if (ExtraTagBitCount > 0) {
        dest.add(unpack.claimAtOffset(getStorageType()->getElementType(1),
                                      CommonSpareBits.size() + offset));
      }
    }
    
    void emitInjection(IRGenFunction &IGF,
                       UnionElementDecl *elt,
                       Explosion &params,
                       Explosion &out) const {
      // See whether this is a payload or empty case we're emitting.
      auto payloadI = std::find_if(PayloadCases.begin(), PayloadCases.end(),
         [&](const std::pair<UnionElementDecl*, const FixedTypeInfo*> &p) {
           return p.first == elt;
         });
      if (payloadI != PayloadCases.end()) {
        return emitPayloadInjection(IGF, elt, *payloadI->second, params, out,
                                    payloadI - PayloadCases.begin());
      }
      auto emptyI = std::find(NoPayloadCases.begin(),NoPayloadCases.end(), elt);
      assert(emptyI != NoPayloadCases.end() && "case not in union");
      emitEmptyInjection(IGF, out, emptyI - NoPayloadCases.begin());
    }
    
  private:
    void emitPayloadInjection(IRGenFunction &IGF, UnionElementDecl *elt,
                              const FixedTypeInfo &payloadTI,
                              Explosion &params, Explosion &out,
                              unsigned tag) const {
      // Pack the payload.
      auto &loadablePayloadTI = cast<LoadableTypeInfo>(payloadTI); // FIXME
      llvm::Value *payload = loadablePayloadTI.packUnionPayload(IGF, params,
                                                     CommonSpareBits.size(), 0);
      
      // If we have spare bits, pack tag bits into them.
      unsigned numSpareBits = CommonSpareBits.count();
      if (numSpareBits > 0) {
        llvm::ConstantInt *tagMask
          = interleaveSpareBits(IGF.IGM, CommonSpareBits,CommonSpareBits.size(),
                                tag, 0);
        payload = IGF.Builder.CreateOr(payload, tagMask);
      }
      
      out.add(payload);
      
      // If we have extra tag bits, pack the remaining tag bits into them.
      if (ExtraTagBitCount > 0) {
        tag >>= numSpareBits;
        auto extra = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                            APInt(ExtraTagBitCount, tag));
        out.add(extra);
      }
    }
    
    void emitEmptyInjection(IRGenFunction &IGF, Explosion &out,
                            unsigned index) const {
      // Figure out the tag and payload for the empty case.
      unsigned numCaseBits = getNumCaseBits();
      unsigned tag, tagIndex;
      if (numCaseBits >= 32) {
        tag = PayloadCases.size();
        tagIndex = index;
      } else {
        tag = (index >> numCaseBits) + PayloadCases.size();
        tagIndex = index & ((1 << numCaseBits) - 1);
      }
      
      llvm::Value *payload;
      unsigned numSpareBits = CommonSpareBits.count();
      if (numSpareBits > 0) {
        // If we have spare bits, pack tag bits into them.
        payload = interleaveSpareBits(IGF.IGM,
                                      CommonSpareBits, CommonSpareBits.size(),
                                      tag, tagIndex);
      } else {
        // Otherwise the payload is just the index.
        payload = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                       APInt(CommonSpareBits.size(), tagIndex));
      }
      
      out.add(payload);
      
      // If we have extra tag bits, pack the remaining tag bits into them.
      if (ExtraTagBitCount > 0) {
        tag >>= numSpareBits;
        auto extra = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                            APInt(ExtraTagBitCount, tag));
        out.add(extra);
      }
    }
    
    void forNontrivialPayloads(IRGenFunction &IGF,
               llvm::Value *payload, llvm::Value *extraTagBits,
               std::function<void (Explosion &payloadValue,
                                   const LoadableTypeInfo &payloadTI)> f) const
    {
      auto *endBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      
      llvm::Value *tag = extractPayloadTag(IGF, payload, extraTagBits);
      auto *swi = IGF.Builder.CreateSwitch(tag, endBB);
      auto *tagTy = cast<llvm::IntegerType>(tag->getType());
      
      // Handle nontrivial tags.
      unsigned tagIndex = 0;
      for (auto &payloadCasePair : PayloadCases) {
        auto &payloadTI = cast<LoadableTypeInfo>(*payloadCasePair.second);
        
        // Trivial payloads don't need any work.
        if (payloadTI.isPOD(ResilienceScope::Local)) {
          ++tagIndex;
          continue;
        }
        
        // Unpack and handle nontrivial payloads.
        auto *caseBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        swi->addCase(llvm::ConstantInt::get(tagTy, tagIndex), caseBB);
        
        IGF.Builder.emitBlock(caseBB);
        Explosion value(ExplosionKind::Minimal);
        projectPayload(IGF, payload, tagIndex, payloadTI, value);
        f(value, payloadTI);
        IGF.Builder.CreateBr(endBB);
        
        ++tagIndex;
      }
      
      IGF.Builder.emitBlock(endBB);
    }
    
  public:
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      if (isPOD(ResilienceScope::Local)) {
        reexplode(IGF, src, dest);
        return;
      }
      
      llvm::Value *payload = src.claimNext();
      llvm::Value *extraTagBits = ExtraTagBitCount > 0
        ? src.claimNext() : nullptr;
      
      forNontrivialPayloads(IGF, payload, extraTagBits,
                            [&](Explosion &value, const LoadableTypeInfo &ti) {
                              Explosion tmp(value.getKind());
                              ti.copy(IGF, value, tmp);
                              tmp.claimAll();
                            });
      
      dest.add(payload);
      if (extraTagBits)
        dest.add(extraTagBits);
    }
    
    void consume(IRGenFunction &IGF, Explosion &src) const override {
      if (isPOD(ResilienceScope::Local)) {
        src.claimAll();
        return;
      }

      llvm::Value *payload = src.claimNext();
      llvm::Value *extraTagBits = ExtraTagBitCount > 0
        ? src.claimNext() : nullptr;
      
      forNontrivialPayloads(IGF, payload, extraTagBits,
                            [&](Explosion &value, const LoadableTypeInfo &ti) {
                              ti.consume(IGF, value);
                            });
    }
    
    void layoutUnionType(TypeConverter &TC, UnionDecl *theUnion,
                         UnionImplStrategy &strategy) override;
  };

  /// A UnionTypeInfo implementation for singleton unions.
  class SingletonUnionTypeInfo : public UnionTypeInfo {
  public:
    static Address getSingletonAddress(IRGenFunction &IGF, Address addr) {
      llvm::Value *singletonAddr =
        IGF.Builder.CreateStructGEP(addr.getAddress(), 0);
      return Address(singletonAddr, addr.getAlignment());
    }

    /// The type info of the singleton member, or null if it carries no data.
    const LoadableTypeInfo *Singleton;

    SingletonUnionTypeInfo(llvm::StructType *T, Size S, Alignment A,
                           IsPOD_t isPOD)
      : UnionTypeInfo(T, S, {}, A, isPOD), Singleton(nullptr) {}

    void getSchema(ExplosionSchema &schema) const {
      assert(isComplete());
      if (Singleton) Singleton->getSchema(schema);
    }

    unsigned getExplosionSize(ExplosionKind kind) const {
      assert(isComplete());
      if (!Singleton) return 0;
      return Singleton->getExplosionSize(kind);
    }

    void loadAsCopy(IRGenFunction &IGF, Address addr, Explosion &e) const {
      if (!Singleton) return;
      Singleton->loadAsCopy(IGF, getSingletonAddress(IGF, addr), e);
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {
      if (!Singleton) return;
      Singleton->loadAsTake(IGF, getSingletonAddress(IGF, addr), e);
    }

    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {
      if (!Singleton) return;
      Singleton->assign(IGF, e, getSingletonAddress(IGF, addr));
    }

    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src) const {
      if (!Singleton) return;
      dest = getSingletonAddress(IGF, dest);
      src = getSingletonAddress(IGF, src);
      Singleton->assignWithCopy(IGF, dest, src);
    }

    void assignWithTake(IRGenFunction &IGF, Address dest, Address src) const {
      if (!Singleton) return;
      dest = getSingletonAddress(IGF, dest);
      src = getSingletonAddress(IGF, src);
      Singleton->assignWithTake(IGF, dest, src);
    }

    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {
      if (!Singleton) return;
      Singleton->initialize(IGF, e, getSingletonAddress(IGF, addr));
    }

    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      if (Singleton) Singleton->reexplode(IGF, src, dest);
    }

    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      if (Singleton) Singleton->copy(IGF, src, dest);
    }

    void consume(IRGenFunction &IGF, Explosion &src) const {
      if (Singleton) Singleton->consume(IGF, src);
    }
    
    void destroy(IRGenFunction &IGF, Address addr) const {
      if (Singleton && !isPOD(ResilienceScope::Local))
        Singleton->destroy(IGF, getSingletonAddress(IGF, addr));
    }
    
    llvm::Value *packUnionPayload(IRGenFunction &IGF,
                                  Explosion &in,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      if (Singleton)
        return Singleton->packUnionPayload(IGF, in, bitWidth, offset);
      return PackUnionPayload::getEmpty(IGF.IGM, bitWidth);
    }
    
    void unpackUnionPayload(IRGenFunction &IGF,
                            llvm::Value *payload,
                            Explosion &dest,
                            unsigned offset) const override {
      if (!Singleton) return;
      Singleton->unpackUnionPayload(IGF, payload, dest, offset);
    }
    
    void emitSwitch(IRGenFunction &IGF,
                    Explosion &value,
                    ArrayRef<std::pair<UnionElementDecl*,
                             llvm::BasicBlock*>> dests,
                    llvm::BasicBlock *defaultDest) const override {
      // No dispatch necessary. Branch straight to the destination.
      value.claimAll();
      
      assert(dests.size() == 1 && "switch table mismatch");
      IGF.Builder.CreateBr(dests[0].second);
    }
    
    void emitProject(IRGenFunction &IGF,
                     Explosion &in,
                     UnionElementDecl *theCase,
                     Explosion &out) const override {
      // The projected value is the payload.
      if (Singleton)
        Singleton->reexplode(IGF, in, out);
    }

    void emitInjection(IRGenFunction &IGF,
                       UnionElementDecl *elt,
                       Explosion &params,
                       Explosion &out) const {
      // If the element carries no data, neither does the injection.
      if (!Singleton) {
        return;
      }

      // Otherwise, package up the result.
      reexplode(IGF, params, out);
    }
    
    void layoutUnionType(TypeConverter &TC, UnionDecl *theUnion,
                         UnionImplStrategy &) override;
  };

  /// A UnionTypeInfo implementation for unions with no payload.
  class NoPayloadUnionTypeInfo :
    public PODSingleScalarTypeInfo<NoPayloadUnionTypeInfo,UnionTypeInfo> {
  public:
    NoPayloadUnionTypeInfo(llvm::StructType *T, Size S, Alignment A)
      : PODSingleScalarTypeInfo(T, S, {}, A) {}

    llvm::Type *getScalarType() const {
      assert(isComplete());
      return getDiscriminatorType();
    }

    static Address projectScalar(IRGenFunction &IGF, Address addr) {
      return IGF.Builder.CreateStructGEP(addr, 0, Size(0));
    }
      
    llvm::IntegerType *getDiscriminatorType() const {
      llvm::StructType *Struct = getStorageType();
      return cast<llvm::IntegerType>(Struct->getElementType(0));
    }
    
    /// Map the given element to the appropriate value in the
    /// discriminator type.
    llvm::ConstantInt *getDiscriminatorIndex(UnionElementDecl *target) const {
      // FIXME: using a linear search here is fairly ridiculous.
      unsigned index = 0;
      for (auto elt : target->getParentUnion()->getAllElements()) {
        if (elt == target) break;
        index++;
      }
      return llvm::ConstantInt::get(getDiscriminatorType(), index);
    }
    
    void emitSwitch(IRGenFunction &IGF,
                    Explosion &value,
                    ArrayRef<std::pair<UnionElementDecl*,
                                       llvm::BasicBlock*>> dests,
                    llvm::BasicBlock *defaultDest) const override {
      llvm::Value *discriminator = value.claimNext();
      
      // Create an unreachable block for the default if the original SIL
      // instruction had none.
      bool unreachableDefault = false;
      if (!defaultDest) {
        unreachableDefault = true;
        defaultDest = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      }
      
      auto *i = IGF.Builder.CreateSwitch(discriminator, defaultDest,
                                         dests.size());
      for (auto &dest : dests)
        i->addCase(getDiscriminatorIndex(dest.first), dest.second);
      
      if (unreachableDefault) {
        IGF.Builder.emitBlock(defaultDest);
        IGF.Builder.CreateUnreachable();
      }
    }
      
    void emitProject(IRGenFunction &IGF,
                     Explosion &in,
                     UnionElementDecl *elt,
                     Explosion &out) const override {
      // All of the cases project an empty explosion.
      in.claimAll();
    }

    void emitInjection(IRGenFunction &IGF,
                       UnionElementDecl *elt,
                       Explosion &params,
                       Explosion &out) const {
      out.add(getDiscriminatorIndex(elt));
    }
    
    void layoutUnionType(TypeConverter &TC, UnionDecl *theUnion,
                         UnionImplStrategy &strategy) override;
  };

  bool isObviouslyEmptyType(CanType type) {
    if (auto tuple = dyn_cast<TupleType>(type)) {
      for (auto eltType : tuple.getElementTypes())
        if (!isObviouslyEmptyType(eltType))
          return false;
      return true;
    }
    // Add more cases here?  Meh.
    return false;
  }

  /// An implementation strategy for a union.
  class UnionImplStrategy {
  public:
    enum Kind {
      /// A "union" with a single case.
      Singleton,
      /// An enum-like union whose cases all have empty payloads.
      NoPayload,
      /// A union which is enum-like except for a single case with payload.
      SinglePayload,
      /// A fully general union with more than one case with payload.
      MultiPayload,
    };

  private:
    unsigned NumElements;
    Kind TheKind;
    std::vector<UnionElementDecl *> ElementsWithPayload;

  public:
    explicit UnionImplStrategy(UnionDecl *theUnion) {
      NumElements = 0;

      for (auto elt : theUnion->getAllElements()) {
        NumElements++;

        // Compute whether this gives us an apparent payload.
        Type argType = elt->getArgumentType();
        if (!argType.isNull() &&
            !isObviouslyEmptyType(argType->getCanonicalType())) {
          ElementsWithPayload.push_back(elt);
        }
      }

      assert(NumElements != 0);
      if (NumElements == 1) {
        TheKind = Singleton;
      } else if (ElementsWithPayload.size() > 1) {
        TheKind = MultiPayload;
      } else if (ElementsWithPayload.size() == 1) {
        TheKind = SinglePayload;
      } else {
        TheKind = NoPayload;
      }
    }

    Kind getKind() const { return TheKind; }
    unsigned getNumElements() const { return NumElements; }
    unsigned getNumElementsWithPayload() const {
      return ElementsWithPayload.size();
    }
    
    UnionElementDecl *getOnlyElementWithPayload() const {
      assert(TheKind == SinglePayload &&
             "not a single-payload union");
      assert(ElementsWithPayload.size() == 1 &&
             "single-payload union has multiple payloads?!");
      return ElementsWithPayload[0];
    }
    
    ArrayRef<UnionElementDecl*> getElementsWithPayload() const {
      return ElementsWithPayload;
    }
    
    /// Create a forward declaration for the union.
    UnionTypeInfo *create(llvm::StructType *convertedStruct) const {
      switch (getKind()) {
      case Singleton:
        return new SingletonUnionTypeInfo(convertedStruct,
                                          Size(0), Alignment(0), IsPOD);
      case NoPayload:
        return new NoPayloadUnionTypeInfo(convertedStruct,
                                          Size(0), Alignment(0));
      
      case SinglePayload:
        return new SinglePayloadUnionTypeInfo(convertedStruct,
                                              Size(0), Alignment(0), IsPOD);
      
      case MultiPayload:
        return new MultiPayloadUnionTypeInfo(convertedStruct,
                                             Size(0), Alignment(0), IsPOD);
      }
    }
  };
  
  void SingletonUnionTypeInfo::layoutUnionType(TypeConverter &TC,
                                               UnionDecl *theUnion,
                                               UnionImplStrategy &) {
    Type eltType =
      theUnion->getAllElements().front()->getArgumentType();
    
    llvm::Type *storageType;
    if (eltType.isNull()) {
      storageType = TC.IGM.Int8Ty;
      completeFixed(Size(0), Alignment(1));
      Singleton = nullptr;
    } else {
      const TypeInfo &eltTI
        = TC.getCompleteTypeInfo(eltType->getCanonicalType());
      
      auto &fixedEltTI = cast<FixedTypeInfo>(eltTI); // FIXME
      storageType = eltTI.StorageType;
      completeFixed(fixedEltTI.getFixedSize(),
                    fixedEltTI.getFixedAlignment());
      Singleton = cast<LoadableTypeInfo>(&eltTI); // FIXME
      setPOD(eltTI.isPOD(ResilienceScope::Local));
    }
    
    llvm::Type *body[] = { storageType };
    getStorageType()->setBody(body);
  }
  
  void NoPayloadUnionTypeInfo::layoutUnionType(TypeConverter &TC,
                                               UnionDecl *theUnion,
                                               UnionImplStrategy &strategy) {
    // Since there are no payloads, we need just enough bits to hold a
    // discriminator.
    assert(strategy.getNumElements() > 1 &&
           "singletons should use SingletonUnionTypeInfo");
    unsigned tagBits = llvm::Log2_32(strategy.getNumElements() - 1) + 1;
    auto tagTy = llvm::IntegerType::get(TC.IGM.getLLVMContext(), tagBits);
    // Round the physical size up to the next power of two.
    unsigned tagBytes = (tagBits + 7U)/8U;
    if (!llvm::isPowerOf2_32(tagBytes))
      tagBytes = llvm::NextPowerOf2(tagBytes);
    completeFixed(Size(tagBytes), Alignment(tagBytes));
    
    llvm::Type *body[] = { tagTy };
    getStorageType()->setBody(body);
  }
  
  static void setTaggedUnionBody(IRGenModule &IGM,
                                 llvm::StructType *bodyStruct,
                                 unsigned payloadBits, unsigned extraTagBits) {

    auto payloadUnionTy = llvm::IntegerType::get(IGM.getLLVMContext(),
                                                 payloadBits);

    if (extraTagBits > 0) {
      auto extraTagTy = llvm::IntegerType::get(IGM.getLLVMContext(),
                                               extraTagBits);
      llvm::Type *body[] = { payloadUnionTy, extraTagTy };
      bodyStruct->setBody(body);
    } else {
      llvm::Type *body[] = { payloadUnionTy };
      bodyStruct->setBody(body);
    }
  }
  
  void SinglePayloadUnionTypeInfo::layoutUnionType(TypeConverter &TC,
                                                   UnionDecl *theUnion,
                                                   UnionImplStrategy &strategy){
    // See whether the payload case's type has extra inhabitants.
    unsigned fixedExtraInhabitants = 0;
    unsigned numTags = strategy.getNumElements() - 1;

    PayloadElement = strategy.getOnlyElementWithPayload();
    auto payloadTy = PayloadElement->getArgumentType()
      ->getCanonicalType();
    
    auto &payloadTI = TC.getCompleteTypeInfo(payloadTy);

    PayloadTypeInfo = cast<FixedTypeInfo>(&payloadTI); // FIXME
    fixedExtraInhabitants = PayloadTypeInfo->getFixedExtraInhabitantCount();
    
    // Determine how many tag bits we need. Given N extra inhabitants, we
    // represent the first N tags using those inhabitants. For additional tags,
    // we use discriminator bit(s) to inhabit the full bit size of the payload.
    unsigned tagsWithoutInhabitants = numTags <= fixedExtraInhabitants
      ? 0 : numTags - fixedExtraInhabitants;
    
    if (tagsWithoutInhabitants == 0) {
      ExtraTagBitCount = 0;
    // If the payload size is greater than 32 bits, the calculation would
    // overflow, but one tag bit should suffice. if you have more than 2^32
    // union discriminators you have other problems.
    } else if (PayloadTypeInfo->getFixedSize().getValue() >= 4) {
      ExtraTagBitCount = 1;
      NumExtraTagValues = 2;
    } else {
      unsigned tagsPerTagBitValue =
        1 << PayloadTypeInfo->getFixedSize().getValueInBits();
      NumExtraTagValues
        = (tagsWithoutInhabitants+(tagsPerTagBitValue-1))/tagsPerTagBitValue+1;
      ExtraTagBitCount = llvm::Log2_32(NumExtraTagValues-1) + 1;
    }

    // Create the body type.
    setTaggedUnionBody(TC.IGM, getStorageType(),
                       PayloadTypeInfo->getFixedSize().getValueInBits(),
                       ExtraTagBitCount);
    
    // The union has the alignment of the payload. The size includes the added
    // tag bits.
    auto sizeWithTag = PayloadTypeInfo->getFixedSize()
      .roundUpToAlignment(PayloadTypeInfo->getFixedAlignment())
      .getValue();
    sizeWithTag += (ExtraTagBitCount+7U)/8U;
    
    setPOD(PayloadTypeInfo->isPOD(ResilienceScope::Component));
    completeFixed(Size(sizeWithTag),
                  PayloadTypeInfo->getFixedAlignment());
  }
  
  void MultiPayloadUnionTypeInfo::layoutUnionType(TypeConverter &TC,
                                                  UnionDecl *theUnion,
                                                  UnionImplStrategy &strategy) {
    // We need tags for each of the payload types, which we may be able to form
    // using spare bits, plus a minimal number of tags with which we can
    // represent the empty cases.
    unsigned numPayloadTags = strategy.getNumElementsWithPayload();
    unsigned numEmptyElements = strategy.getNumElements() - numPayloadTags;
    
    // See if the payload types have any spare bits in common.
    // At the end of the loop CommonSpareBits.size() will be the size (in bits)
    // of the largest payload.
    CommonSpareBits = {};
    Alignment worstAlignment(1);
    IsPOD_t isPOD = IsPOD;
    for (auto *elt : strategy.getElementsWithPayload()) {
      auto payloadTy = elt->getArgumentType()->getCanonicalType();
      auto &payloadTI = TC.getCompleteTypeInfo(payloadTy);
      auto &fixedPayloadTI = cast<FixedTypeInfo>(payloadTI); // FIXME
      fixedPayloadTI.applyFixedSpareBitsMask(CommonSpareBits);
      if (fixedPayloadTI.getFixedAlignment() > worstAlignment)
        worstAlignment = fixedPayloadTI.getFixedAlignment();
      if (!fixedPayloadTI.isPOD(ResilienceScope::Component))
        isPOD = IsNotPOD;
      PayloadCases.emplace_back(elt, &fixedPayloadTI);
    }
    
    // Collect the no-payload cases.
    // FIXME: O(n^2)
    for (auto *elt : theUnion->getAllElements()) {
      if (std::find(strategy.getElementsWithPayload().begin(),
                    strategy.getElementsWithPayload().end(),
                    elt) == strategy.getElementsWithPayload().end())
        NoPayloadCases.push_back(elt);
    }
    
    unsigned commonSpareBitCount = CommonSpareBits.count();
    unsigned usedBitCount = CommonSpareBits.size() - commonSpareBitCount;
    
    // We can store tags for the empty elements using the inhabited bits with
    // their own tag(s).
    if (usedBitCount >= 32) {
      NumEmptyElementTags = 1;
    } else {
      unsigned emptyElementsPerTag = 1 << usedBitCount;
      NumEmptyElementTags
        = (numEmptyElements + (emptyElementsPerTag-1))/emptyElementsPerTag;
    }
    
    unsigned numTags = numPayloadTags + NumEmptyElementTags;
    unsigned numTagBits = llvm::Log2_32(numTags-1) + 1;
    ExtraTagBitCount = numTagBits <= commonSpareBitCount
      ? 0 : numTagBits - commonSpareBitCount;
    NumExtraTagValues = numTags >> commonSpareBitCount;
    
    // Create the type. We need enough bits to store the largest payload plus
    // extra tag bits we need.
    setTaggedUnionBody(TC.IGM, getStorageType(),
                       CommonSpareBits.size(),
                       ExtraTagBitCount);
    
    // The union has the worst alignment of its payloads. The size includes the
    // added tag bits.
    auto sizeWithTag = Size((CommonSpareBits.size() + 7U)/8U)
      .roundUpToAlignment(worstAlignment)
      .getValue();
    sizeWithTag += (ExtraTagBitCount+7U)/8U;
    
    setPOD(isPOD);
    completeFixed(Size(sizeWithTag), worstAlignment);
  }
}

const TypeInfo *TypeConverter::convertUnionType(UnionDecl *theUnion) {
  llvm::StructType *convertedStruct = IGM.createNominalType(theUnion);

  // Create a forward declaration for that type.
  auto typeCacheKey = theUnion->getDeclaredType().getPointer();
  addForwardDecl(typeCacheKey, convertedStruct);

  // Compute the implementation strategy.
  UnionImplStrategy strategy(theUnion);

  // Create the TI as a forward declaration and map it in the table.
  UnionTypeInfo *convertedTI = strategy.create(convertedStruct);

  convertedTI->layoutUnionType(*this, theUnion, strategy);
  return convertedTI;
}

/// emitUnionDecl - Emit all the declarations associated with this union type.
void IRGenModule::emitUnionDecl(UnionDecl *theUnion) {
  emitUnionMetadata(*this, theUnion);
  
  // FIXME: This is mostly copy-paste from emitExtension;
  // figure out how to refactor! 
  for (Decl *member : theUnion->getMembers()) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::Extension:
    case DeclKind::Destructor:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
      llvm_unreachable("decl not allowed in struct!");

    // We can't have meaningful initializers for variables; these just show
    // up as part of parsing properties.
    case DeclKind::PatternBinding:
      continue;

    case DeclKind::Subscript:
      // Getter/setter will be handled separately.
      continue;
    case DeclKind::TypeAlias:
    case DeclKind::AssociatedType:
    case DeclKind::GenericTypeParam:
      continue;
    case DeclKind::Union:
      emitUnionDecl(cast<UnionDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    case DeclKind::Var:
      if (cast<VarDecl>(member)->isProperty())
        // Getter/setter will be handled separately.
        continue;
      // FIXME: Will need an implementation here for resilience
      continue;
    case DeclKind::Func:
      emitLocalDecls(cast<FuncDecl>(member));
      continue;
    case DeclKind::Constructor:
      emitLocalDecls(cast<ConstructorDecl>(member));
      continue;
    case DeclKind::UnionElement:
      // Lowered in SIL.
      continue;
    }
    llvm_unreachable("bad extension member kind");
  }
}

// FIXME: PackUnionPayload and UnpackUnionPayload need to be endian-aware.

PackUnionPayload::PackUnionPayload(IRGenFunction &IGF, unsigned bitSize)
  : IGF(IGF), bitSize(bitSize)
{}

void PackUnionPayload::add(llvm::Value *v) {
  // First, bitcast to an integer type.
  if (isa<llvm::PointerType>(v->getType())) {
    v = IGF.Builder.CreatePtrToInt(v, IGF.IGM.SizeTy);
  } else if (!isa<llvm::IntegerType>(v->getType())) {
    unsigned bitSize = IGF.IGM.DataLayout.getTypeSizeInBits(v->getType());
    auto intTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), bitSize);
    v = IGF.Builder.CreateBitCast(v, intTy);
  }
  auto fromTy = cast<llvm::IntegerType>(v->getType());
  
  // If this was the first added value, use it to start our packed value.
  if (!packedValue) {
    // Zero-extend the integer value out to the value size.
    // FIXME: On big-endian, shift out to the value size.
    if (fromTy->getBitWidth() < bitSize) {
      auto toTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), bitSize);
      v = IGF.Builder.CreateZExt(v, toTy);
    }
    if (packedBits != 0)
      v = IGF.Builder.CreateShl(v, packedBits);
    packedBits += fromTy->getBitWidth();
    packedValue = v;
    return;
  }
  
  // Otherwise, shift and bitor the value into the existing value.
  v = IGF.Builder.CreateZExt(v, packedValue->getType());
  v = IGF.Builder.CreateShl(v, packedBits);
  packedBits += fromTy->getBitWidth();
  packedValue = IGF.Builder.CreateOr(packedValue, v);
}

void PackUnionPayload::addAtOffset(llvm::Value *v, unsigned bitOffset) {
  packedBits = bitOffset;
  add(v);
}

void PackUnionPayload::combine(llvm::Value *v) {
  if (!packedValue)
    packedValue = v;
  else
    packedValue = IGF.Builder.CreateOr(packedValue, v);
}

llvm::Value *PackUnionPayload::get() {
  if (!packedValue)
    packedValue = getEmpty(IGF.IGM, bitSize);
  return packedValue;
}

llvm::Value *PackUnionPayload::getEmpty(IRGenModule &IGM, unsigned bitSize) {
  return llvm::ConstantInt::get(IGM.getLLVMContext(), APInt(bitSize, 0));
}

UnpackUnionPayload::UnpackUnionPayload(IRGenFunction &IGF,
                                       llvm::Value *packedValue)
  : IGF(IGF), packedValue(packedValue)
{}

llvm::Value *UnpackUnionPayload::claim(llvm::Type *ty) {
  // Mask out the bits for the value.
  unsigned bitSize = IGF.IGM.DataLayout.getTypeSizeInBits(ty);
  auto bitTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), bitSize);
  llvm::Value *unpacked = unpackedBits == 0
    ? packedValue
    : IGF.Builder.CreateLShr(packedValue, unpackedBits);
  if (bitSize < cast<llvm::IntegerType>(packedValue->getType())->getBitWidth())
    unpacked = IGF.Builder.CreateTrunc(unpacked, bitTy);

  unpackedBits += bitSize;

  // Bitcast to the destination type.
  if (isa<llvm::PointerType>(ty))
    return IGF.Builder.CreateIntToPtr(unpacked, ty);
  return IGF.Builder.CreateBitCast(unpacked, ty);
}

llvm::Value *UnpackUnionPayload::claimAtOffset(llvm::Type *ty,
                                               unsigned bitOffset) {
  unpackedBits = bitOffset;
  return claim(ty);
}

void irgen::emitSwitchLoadableUnionDispatch(IRGenFunction &IGF,
                                  SILType unionTy,
                                  Explosion &unionValue,
                                  ArrayRef<std::pair<UnionElementDecl *,
                                                     llvm::BasicBlock *>> dests,
                                  llvm::BasicBlock *defaultDest) {
  assert(unionTy.getSwiftRValueType()->getUnionOrBoundGenericUnion()
         && "not of a union type");
  auto &unionTI = IGF.getTypeInfo(unionTy).as<UnionTypeInfo>();
  unionTI.emitSwitch(IGF, unionValue, dests, defaultDest);
}

void irgen::emitInjectLoadableUnion(IRGenFunction &IGF, SILType unionTy,
                                    UnionElementDecl *theCase,
                                    Explosion &data,
                                    Explosion &out) {
  assert(unionTy.getSwiftRValueType()->getUnionOrBoundGenericUnion()
         && "not of a union type");
  auto &unionTI = IGF.getTypeInfo(unionTy).as<UnionTypeInfo>();
  unionTI.emitInjection(IGF, theCase, data, out);
}

void irgen::emitProjectLoadableUnion(IRGenFunction &IGF, SILType unionTy,
                                     Explosion &inUnionValue,
                                     UnionElementDecl *theCase,
                                     Explosion &out) {
  assert(unionTy.getSwiftRValueType()->getUnionOrBoundGenericUnion()
         && "not of a union type");
  auto &unionTI = IGF.getTypeInfo(unionTy).as<UnionTypeInfo>();
  unionTI.emitProject(IGF, inUnionValue, theCase, out);
}

/// Interleave the occupiedValue and spareValue bits, taking a bit from one
/// or the other at each position based on the spareBits mask.
llvm::ConstantInt *
irgen::interleaveSpareBits(IRGenModule &IGM, const llvm::BitVector &spareBits,
                           unsigned bits,
                           unsigned spareValue, unsigned occupiedValue) {
  // FIXME: endianness.
  SmallVector<llvm::integerPart, 2> valueParts;
  valueParts.push_back(0);
  
  llvm::integerPart valueBit = 1;
  auto advanceValueBit = [&]{
    valueBit <<= 1;
    if (valueBit == 0) {
      valueParts.push_back(0);
      valueBit = 1;
    }
  };
  
  for (unsigned i = 0, e = spareBits.size();
       (occupiedValue || spareValue) && i < e;
       ++i, advanceValueBit()) {
    if (spareBits[i]) {
      if (spareValue & 1)
      valueParts.back() |= valueBit;
      spareValue >>= 1;
    } else {
      if (occupiedValue & 1)
      valueParts.back() |= valueBit;
      occupiedValue >>= 1;
    }
  }
  
  // Create the value.
  llvm::APInt value(bits, valueParts);
  return llvm::ConstantInt::get(IGM.getLLVMContext(), value);
}
