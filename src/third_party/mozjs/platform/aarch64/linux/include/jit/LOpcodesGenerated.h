/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_LOpcodesGenerated_h
#define jit_LOpcodesGenerated_h

/* This file is generated by jit/GenerateOpcodeFiles.py. Do not edit! */

#define LIR_OPCODE_LIST(_) \
_(Phi)\
_(Box)\
_(OsiPoint)\
_(MoveGroup)\
_(Integer)\
_(Integer64)\
_(Pointer)\
_(Double)\
_(Float32)\
_(Value)\
_(NurseryObject)\
_(Parameter)\
_(Callee)\
_(IsConstructing)\
_(Goto)\
_(NewArray)\
_(NewArrayDynamicLength)\
_(NewIterator)\
_(NewTypedArray)\
_(NewTypedArrayDynamicLength)\
_(NewTypedArrayFromArray)\
_(NewTypedArrayFromArrayBuffer)\
_(NewObject)\
_(NewPlainObject)\
_(NewArrayObject)\
_(NewNamedLambdaObject)\
_(NewCallObject)\
_(NewStringObject)\
_(InitElemGetterSetter)\
_(MutateProto)\
_(InitPropGetterSetter)\
_(CheckOverRecursed)\
_(WasmTrap)\
_(WasmReinterpret)\
_(WasmReinterpretFromI64)\
_(WasmReinterpretToI64)\
_(Rotate)\
_(RotateI64)\
_(InterruptCheck)\
_(WasmInterruptCheck)\
_(TypeOfV)\
_(TypeOfO)\
_(ToAsyncIter)\
_(ToPropertyKeyCache)\
_(CreateThis)\
_(CreateThisWithTemplate)\
_(CreateArgumentsObject)\
_(CreateInlinedArgumentsObject)\
_(GetInlinedArgument)\
_(GetArgumentsObjectArg)\
_(SetArgumentsObjectArg)\
_(LoadArgumentsObjectArg)\
_(ArgumentsObjectLength)\
_(GuardArgumentsObjectFlags)\
_(ReturnFromCtor)\
_(BoxNonStrictThis)\
_(ImplicitThis)\
_(StackArgT)\
_(StackArgV)\
_(CallGeneric)\
_(CallKnown)\
_(CallNative)\
_(CallDOMNative)\
_(Bail)\
_(Unreachable)\
_(EncodeSnapshot)\
_(UnreachableResultV)\
_(UnreachableResultT)\
_(GetDOMProperty)\
_(GetDOMMemberV)\
_(GetDOMMemberT)\
_(SetDOMProperty)\
_(LoadDOMExpandoValue)\
_(LoadDOMExpandoValueGuardGeneration)\
_(LoadDOMExpandoValueIgnoreGeneration)\
_(GuardDOMExpandoMissingOrGuardShape)\
_(ApplyArgsGeneric)\
_(ApplyArgsObj)\
_(ApplyArrayGeneric)\
_(ConstructArrayGeneric)\
_(TestIAndBranch)\
_(TestI64AndBranch)\
_(TestDAndBranch)\
_(TestFAndBranch)\
_(TestBIAndBranch)\
_(TestOAndBranch)\
_(TestVAndBranch)\
_(Compare)\
_(CompareI64)\
_(CompareI64AndBranch)\
_(CompareAndBranch)\
_(CompareD)\
_(CompareF)\
_(CompareDAndBranch)\
_(CompareFAndBranch)\
_(CompareS)\
_(CompareBigInt)\
_(CompareBigIntInt32)\
_(CompareBigIntDouble)\
_(CompareBigIntString)\
_(BitAndAndBranch)\
_(IsNullOrLikeUndefinedV)\
_(IsNullOrLikeUndefinedT)\
_(IsNullOrLikeUndefinedAndBranchV)\
_(IsNullOrLikeUndefinedAndBranchT)\
_(SameValueDouble)\
_(SameValue)\
_(NotI)\
_(NotI64)\
_(NotD)\
_(NotF)\
_(NotBI)\
_(NotO)\
_(NotV)\
_(BitNotI)\
_(BitOpI)\
_(BitOpI64)\
_(ShiftI)\
_(ShiftI64)\
_(SignExtendInt32)\
_(SignExtendInt64)\
_(UrshD)\
_(Return)\
_(Throw)\
_(MinMaxI)\
_(MinMaxD)\
_(MinMaxF)\
_(MinMaxArrayI)\
_(MinMaxArrayD)\
_(NegI)\
_(NegI64)\
_(NegD)\
_(NegF)\
_(AbsI)\
_(AbsD)\
_(AbsF)\
_(CopySignD)\
_(CopySignF)\
_(ClzI)\
_(ClzI64)\
_(CtzI)\
_(CtzI64)\
_(PopcntI)\
_(PopcntI64)\
_(SqrtD)\
_(SqrtF)\
_(Atan2D)\
_(Hypot)\
_(PowI)\
_(PowII)\
_(PowD)\
_(PowOfTwoI)\
_(SignI)\
_(SignD)\
_(SignDI)\
_(MathFunctionD)\
_(MathFunctionF)\
_(AddI)\
_(AddI64)\
_(SubI)\
_(SubI64)\
_(MulI64)\
_(MathD)\
_(MathF)\
_(ModD)\
_(ModPowTwoD)\
_(WasmBuiltinModD)\
_(BigIntAdd)\
_(BigIntSub)\
_(BigIntMul)\
_(BigIntDiv)\
_(BigIntMod)\
_(BigIntPow)\
_(BigIntBitAnd)\
_(BigIntBitOr)\
_(BigIntBitXor)\
_(BigIntLsh)\
_(BigIntRsh)\
_(BigIntIncrement)\
_(BigIntDecrement)\
_(BigIntNegate)\
_(BigIntBitNot)\
_(Concat)\
_(CharCodeAt)\
_(FromCharCode)\
_(FromCodePoint)\
_(StringConvertCase)\
_(StringSplit)\
_(Substr)\
_(Int32ToDouble)\
_(Float32ToDouble)\
_(DoubleToFloat32)\
_(Int32ToFloat32)\
_(ValueToDouble)\
_(ValueToFloat32)\
_(ValueToInt32)\
_(ValueToBigInt)\
_(DoubleToInt32)\
_(Float32ToInt32)\
_(DoubleToIntegerInt32)\
_(Float32ToIntegerInt32)\
_(TruncateDToInt32)\
_(WasmBuiltinTruncateDToInt32)\
_(TruncateFToInt32)\
_(WasmBuiltinTruncateFToInt32)\
_(WasmTruncateToInt32)\
_(WrapInt64ToInt32)\
_(ExtendInt32ToInt64)\
_(BooleanToString)\
_(IntToString)\
_(DoubleToString)\
_(ValueToString)\
_(PowHalfD)\
_(NaNToZero)\
_(OsrEntry)\
_(OsrValue)\
_(OsrEnvironmentChain)\
_(OsrReturnValue)\
_(OsrArgumentsObject)\
_(RegExp)\
_(RegExpMatcher)\
_(RegExpSearcher)\
_(RegExpTester)\
_(RegExpPrototypeOptimizable)\
_(RegExpInstanceOptimizable)\
_(GetFirstDollarIndex)\
_(StringReplace)\
_(BinaryValueCache)\
_(BinaryBoolCache)\
_(UnaryCache)\
_(ModuleMetadata)\
_(DynamicImport)\
_(Lambda)\
_(LambdaArrow)\
_(FunctionWithProto)\
_(SetFunName)\
_(KeepAliveObject)\
_(Slots)\
_(Elements)\
_(InitializedLength)\
_(SetInitializedLength)\
_(ArrayLength)\
_(SetArrayLength)\
_(FunctionLength)\
_(FunctionName)\
_(GetNextEntryForIterator)\
_(ArrayBufferByteLength)\
_(ArrayBufferViewLength)\
_(ArrayBufferViewByteOffset)\
_(ArrayBufferViewElements)\
_(TypedArrayElementSize)\
_(GuardHasAttachedArrayBuffer)\
_(GuardNumberToIntPtrIndex)\
_(BoundsCheck)\
_(BoundsCheckRange)\
_(BoundsCheckLower)\
_(SpectreMaskIndex)\
_(LoadElementV)\
_(InArray)\
_(GuardElementNotHole)\
_(LoadElementHole)\
_(StoreElementV)\
_(StoreElementT)\
_(StoreHoleValueElement)\
_(StoreElementHoleV)\
_(StoreElementHoleT)\
_(ArrayPopShift)\
_(ArrayPush)\
_(ArraySlice)\
_(ArrayJoin)\
_(LoadUnboxedScalar)\
_(LoadUnboxedBigInt)\
_(LoadDataViewElement)\
_(LoadTypedArrayElementHole)\
_(LoadTypedArrayElementHoleBigInt)\
_(StoreUnboxedScalar)\
_(StoreUnboxedBigInt)\
_(StoreDataViewElement)\
_(StoreTypedArrayElementHole)\
_(StoreTypedArrayElementHoleBigInt)\
_(AtomicIsLockFree)\
_(CompareExchangeTypedArrayElement)\
_(AtomicExchangeTypedArrayElement)\
_(AtomicTypedArrayElementBinop)\
_(AtomicTypedArrayElementBinopForEffect)\
_(AtomicLoad64)\
_(AtomicStore64)\
_(CompareExchangeTypedArrayElement64)\
_(AtomicExchangeTypedArrayElement64)\
_(AtomicTypedArrayElementBinop64)\
_(AtomicTypedArrayElementBinopForEffect64)\
_(EffectiveAddress)\
_(ClampIToUint8)\
_(ClampDToUint8)\
_(ClampVToUint8)\
_(LoadFixedSlotV)\
_(LoadFixedSlotT)\
_(LoadFixedSlotAndUnbox)\
_(LoadDynamicSlotAndUnbox)\
_(LoadElementAndUnbox)\
_(AddAndStoreSlot)\
_(AllocateAndStoreSlot)\
_(StoreFixedSlotV)\
_(StoreFixedSlotT)\
_(GetNameCache)\
_(CallGetIntrinsicValue)\
_(GetPropSuperCache)\
_(GetPropertyCache)\
_(BindNameCache)\
_(CallBindVar)\
_(LoadDynamicSlotV)\
_(StoreDynamicSlotV)\
_(StoreDynamicSlotT)\
_(StringLength)\
_(Floor)\
_(FloorF)\
_(Ceil)\
_(CeilF)\
_(Round)\
_(RoundF)\
_(Trunc)\
_(TruncF)\
_(NearbyInt)\
_(NearbyIntF)\
_(FunctionEnvironment)\
_(HomeObject)\
_(HomeObjectSuperBase)\
_(NewLexicalEnvironmentObject)\
_(CopyLexicalEnvironmentObject)\
_(NewClassBodyEnvironmentObject)\
_(CallSetElement)\
_(CallDeleteProperty)\
_(CallDeleteElement)\
_(SetPropertyCache)\
_(GetIteratorCache)\
_(OptimizeSpreadCallCache)\
_(IteratorMore)\
_(IsNoIterAndBranch)\
_(IteratorEnd)\
_(ArgumentsLength)\
_(GetFrameArgument)\
_(Rest)\
_(Int32ToIntPtr)\
_(NonNegativeIntPtrToInt32)\
_(IntPtrToDouble)\
_(AdjustDataViewLength)\
_(BooleanToInt64)\
_(StringToInt64)\
_(ValueToInt64)\
_(TruncateBigIntToInt64)\
_(Int64ToBigInt)\
_(PostWriteBarrierO)\
_(PostWriteBarrierS)\
_(PostWriteBarrierBI)\
_(PostWriteBarrierV)\
_(PostWriteElementBarrierO)\
_(PostWriteElementBarrierS)\
_(PostWriteElementBarrierBI)\
_(PostWriteElementBarrierV)\
_(GuardObjectIdentity)\
_(GuardSpecificFunction)\
_(GuardSpecificAtom)\
_(GuardSpecificSymbol)\
_(GuardStringToIndex)\
_(GuardStringToInt32)\
_(GuardStringToDouble)\
_(GuardShape)\
_(GuardProto)\
_(GuardNullProto)\
_(GuardIsNativeObject)\
_(GuardIsProxy)\
_(GuardIsNotProxy)\
_(GuardIsNotDOMProxy)\
_(ProxyGet)\
_(ProxyGetByValue)\
_(ProxyHasProp)\
_(ProxySet)\
_(ProxySetByValue)\
_(CallSetArrayLength)\
_(MegamorphicLoadSlot)\
_(MegamorphicLoadSlotByValue)\
_(MegamorphicStoreSlot)\
_(MegamorphicHasProp)\
_(GuardIsNotArrayBufferMaybeShared)\
_(GuardIsTypedArray)\
_(GuardNoDenseElements)\
_(InCache)\
_(HasOwnCache)\
_(CheckPrivateFieldCache)\
_(InstanceOfO)\
_(InstanceOfV)\
_(InstanceOfCache)\
_(IsCallableO)\
_(IsCallableV)\
_(IsConstructor)\
_(IsCrossRealmArrayConstructor)\
_(IsArrayO)\
_(IsArrayV)\
_(IsTypedArray)\
_(IsObject)\
_(IsObjectAndBranch)\
_(IsNullOrUndefined)\
_(IsNullOrUndefinedAndBranch)\
_(HasClass)\
_(GuardToClass)\
_(ObjectClassToString)\
_(WasmSelect)\
_(WasmSelectI64)\
_(WasmCompareAndSelect)\
_(WasmAddOffset)\
_(WasmBoundsCheck)\
_(WasmBoundsCheck64)\
_(WasmExtendU32Index)\
_(WasmWrapU32Index)\
_(WasmAlignmentCheck)\
_(WasmLoadTls)\
_(WasmHeapBase)\
_(WasmLoad)\
_(WasmLoadI64)\
_(WasmStore)\
_(WasmStoreI64)\
_(AsmJSLoadHeap)\
_(AsmJSStoreHeap)\
_(WasmCompareExchangeHeap)\
_(WasmFence)\
_(WasmAtomicExchangeHeap)\
_(WasmAtomicBinopHeap)\
_(WasmAtomicBinopHeapForEffect)\
_(WasmLoadSlot)\
_(WasmLoadSlotI64)\
_(WasmStoreSlot)\
_(WasmStoreSlotI64)\
_(WasmDerivedPointer)\
_(WasmStoreRef)\
_(WasmParameter)\
_(WasmParameterI64)\
_(WasmReturn)\
_(WasmReturnI64)\
_(WasmReturnVoid)\
_(WasmStackArg)\
_(WasmStackArgI64)\
_(WasmNullConstant)\
_(WasmCall)\
_(WasmRegisterResult)\
_(WasmRegisterPairResult)\
_(WasmStackResultArea)\
_(WasmStackResult)\
_(WasmStackResult64)\
_(AssertRangeI)\
_(AssertRangeD)\
_(AssertRangeF)\
_(AssertRangeV)\
_(AssertClass)\
_(AssertShape)\
_(AssertResultT)\
_(AssertResultV)\
_(GuardValue)\
_(GuardNullOrUndefined)\
_(GuardFunctionFlags)\
_(GuardFunctionIsNonBuiltinCtor)\
_(GuardFunctionKind)\
_(GuardFunctionScript)\
_(IncrementWarmUpCounter)\
_(LexicalCheck)\
_(ThrowRuntimeLexicalError)\
_(ThrowMsg)\
_(GlobalDeclInstantiation)\
_(MemoryBarrier)\
_(Debugger)\
_(NewTarget)\
_(ArrowNewTarget)\
_(Random)\
_(CheckReturn)\
_(CheckIsObj)\
_(CheckObjCoercible)\
_(CheckClassHeritage)\
_(CheckThis)\
_(CheckThisReinit)\
_(Generator)\
_(AsyncResolve)\
_(AsyncAwait)\
_(CanSkipAwait)\
_(MaybeExtractAwaitValue)\
_(DebugCheckSelfHosted)\
_(FinishBoundFunctionInit)\
_(IsPackedArray)\
_(GuardArrayIsPacked)\
_(GetPrototypeOf)\
_(ObjectWithProto)\
_(ObjectStaticProto)\
_(BuiltinObject)\
_(SuperFunction)\
_(InitHomeObject)\
_(IsTypedArrayConstructor)\
_(LoadValueTag)\
_(GuardTagNotEqual)\
_(LoadWrapperTarget)\
_(GuardHasGetterSetter)\
_(GuardIsExtensible)\
_(GuardInt32IsNonNegative)\
_(GuardIndexGreaterThanDenseInitLength)\
_(GuardIndexIsValidUpdateOrAdd)\
_(CallAddOrUpdateSparseElement)\
_(CallGetSparseElement)\
_(CallNativeGetElement)\
_(CallObjectHasSparseElement)\
_(BigIntAsIntN)\
_(BigIntAsIntN64)\
_(BigIntAsIntN32)\
_(BigIntAsUintN)\
_(BigIntAsUintN64)\
_(BigIntAsUintN32)\
_(IonToWasmCall)\
_(IonToWasmCallV)\
_(IonToWasmCallI64)\
_(WasmBoxValue)\
_(WasmAnyRefFromJSObject)\
_(Simd128)\
_(WasmBitselectSimd128)\
_(WasmBinarySimd128)\
_(WasmBinarySimd128WithConstant)\
_(WasmVariableShiftSimd128)\
_(WasmConstantShiftSimd128)\
_(WasmSignReplicationSimd128)\
_(WasmShuffleSimd128)\
_(WasmPermuteSimd128)\
_(WasmReplaceLaneSimd128)\
_(WasmReplaceInt64LaneSimd128)\
_(WasmScalarToSimd128)\
_(WasmInt64ToSimd128)\
_(WasmUnarySimd128)\
_(WasmReduceSimd128)\
_(WasmReduceAndBranchSimd128)\
_(WasmReduceSimd128ToInt64)\
_(WasmLoadLaneSimd128)\
_(WasmStoreLaneSimd128)\
_(Unbox)\
_(UnboxFloatingPoint)\
_(WasmUint32ToDouble)\
_(WasmUint32ToFloat32)\
_(DivI)\
_(DivPowTwoI)\
_(DivConstantI)\
_(UDivConstantI)\
_(ModI)\
_(ModPowTwoI)\
_(ModMaskI)\
_(TableSwitch)\
_(TableSwitchV)\
_(MulI)\
_(UDiv)\
_(UMod)\
_(Int64ToFloatingPoint)\
_(WasmTruncateToInt64)\
_(DivOrModI64)\
_(UDivOrModI64)

#endif // jit_LOpcodesGenerated_h
