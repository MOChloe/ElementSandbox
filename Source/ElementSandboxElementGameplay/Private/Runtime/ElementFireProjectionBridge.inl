	void ApplyStructuralCommands()
	{
		TArray<FElementStructuralCommand> Commands;
		Execution->ConsumeStructuralCommands(Commands);
		for (const FElementStructuralCommand& Command : Commands)
		{
			if (Command.FragmentType != ElementFireRuntimeNames::FireSourceFragment) continue;
			FHostRecord* Host = Hosts.Find(Command.Target);
			if (!Host) continue;
			if (Command.Kind == EElementStructuralCommandKind::RemoveInfluenceFragment)
			{
				if (FPersistentBinding* Binding = FindPersistentBinding(Command.Target);
					Binding && Binding->bHasFireSource && Execution->ReadRegistry().IsAlive(Binding->Element))
				{
					Execution->RemoveFragment<FFireSourceFragment>(Binding->Element);
					Binding->bHasFireSource = false;
					PendingPersistenceDirty.Add(Command.Target);
				}
				continue;
			}
			if (Command.Kind != EElementStructuralCommandKind::AddInfluenceFragment
				|| Command.Payload.Count < 3) continue;
			const double Intensity = Command.Payload.Values[0];
			const double Range = Command.Payload.Values[1];
			const int32 Policy = FMath::RoundToInt(Command.Payload.Values[2]);
			if (!FMath::IsFinite(Intensity) || Intensity <= 0.0
				|| !FMath::IsFinite(Range) || Range <= 0.0
				|| Policy < static_cast<int32>(EFirePropagationPolicy::All)
				|| Policy > static_cast<int32>(EFirePropagationPolicy::CharacterOnly)) continue;
			FFireSourceFragment Fragment;
			Fragment.Shape = Host->MakeCompoundShape();
			Fragment.Intensity = Intensity;
			Fragment.RangeCentimeters = Range;
			Fragment.Policy = static_cast<EFirePropagationPolicy>(Policy);
			Fragment.HostTarget = Host->Target;
			if (!Fragment.IsValid()) continue;
			FPersistentBinding* Binding = EnsurePersistentBinding(*Host);
			if (!Binding || !Execution->ReadRegistry().IsAlive(Binding->Element)) continue;
			if (Binding->bHasFireSource)
			{
				Execution->EditFragment<FFireSourceFragment>(Binding->Element,
					[&Fragment](FFireSourceFragment& Current)
					{
						Current.Shape = Fragment.Shape;
						Current.Intensity = Fragment.Intensity;
						Current.RangeCentimeters = Fragment.RangeCentimeters;
						Current.Policy = Fragment.Policy;
						Current.HostTarget = Fragment.HostTarget;
					});
			}
			else if (Execution->AddFragment(Binding->Element, Fragment))
			{
				Binding->bHasFireSource = true;
			}
			PendingPersistenceDirty.Add(Command.Target);
		}
	}

	bool ApplyProjectionCommand(const FElementProjectionCommand& Command)
	{
		if (Command.Channel != ElementFireRuntimeNames::Projection || Command.Payload.Count < 6) return false;
		FHostRecord* Host = Hosts.Find(Command.Target);
		if (!Host || Command.Revision <= Host->LastProjectionRevision) return false;
		Host->LastProjectionRevision = Command.Revision;
		const EFireCombustionPhase Phase = ReadProjectionPhase(Command);
		if (Host->Profile == EElementFireTargetProfile::Character)
		{
			ProjectCharacter(*Host, Phase, FMath::RoundToInt(Command.Payload.Values[3]));
		}
		else if (Host->Profile == EElementFireTargetProfile::Structure
			&& (Host->Building.IsSet() || Host->WorldObject.IsSet()))
		{
			UpdateBurnPresentation(*Host, Command, Phase);
		}
		else if (Host->Profile == EElementFireTargetProfile::Stick)
		{
			ProjectStick(*Host, Phase == EFireCombustionPhase::Burning);
		}
		ProjectVisual(*Host, Command, Phase);
		if (Phase == EFireCombustionPhase::BurnedOut
			&& Host->Profile == EElementFireTargetProfile::Structure
			&& (Host->Building.IsSet() || Host->WorldObject.IsSet()))
		{
			PendingBurnoutConversions.FindOrAdd(Host->Target) = Command.Revision;
		}
		return true;
	}

	void ApplyProjectionCommands()
	{
		TArray<FElementProjectionCommand> Commands;
		Execution->ConsumeProjectionCommands(Commands);
		for (const FElementProjectionCommand& Command : Commands)
		{
			FHostRecord* Host = Hosts.Find(Command.Target);
			if (!Host || !ApplyProjectionCommand(Command) || Host->Target.Domain == EElementTargetDomain::Character)
				continue;
			FElementAuthorityTargetStateSnapshot State;
			if (!Execution->CaptureTargetState(Host->Target, State)) continue;
			if (ShouldPersistAuthorityState(State))
			{
				if (EnsurePersistentBinding(*Host)) PendingPersistenceDirty.Add(Host->Target);
			}
			else
			{
				DestroyPersistentBinding(Host->Target, true);
			}
		}
	}

	bool TryConvertBurnedOutTarget(
		const FElementTargetKey Target,
		const uint64 ProjectionRevision)
	{
		const FHostRecord* Host = Hosts.Find(Target);
		if (!Host || Host->Profile != EElementFireTargetProfile::Structure
			|| !Host->WorldEntityId.IsSet() || !WorldObjects.IsValid())
		{
			return true;
		}

		const FWorldEntityId SourceId = Host->WorldEntityId;
                const FBuildEntityHandle Building = Host->Building;
                const FWorldObjectEntityHandle WorldObject = Host->WorldObject;
                FBox SourceBounds(ForceInit);
				UE::ElementSandbox::Destruction::FWorldDestructionProductBatch Batch;
				Batch.SourceId = SourceId;
				Batch.DestructionRevision = FMath::Max<uint64>(1, ProjectionRevision);
				Batch.Definition = &GetBurnoutProducts();
				Batch.Target.WorldEntityId = SourceId;
				Batch.Target.SourceRevision = static_cast<uint32>(FMath::Clamp<uint64>(
					Batch.DestructionRevision, 1, MAX_uint32));
                if (Building.IsSet())
                {
			if (!Buildings.IsValid() || !Buildings->IsEntityAlive(Building)
				|| Buildings->GetWorldEntityId(Building) != SourceId)
			{
				return true;
			}
			if (!Buildings->GetSpatialIndex().TryGetBounds(Building, SourceBounds))
                        {
                                return false;
                        }
						Batch.Target.Domain = UE::ElementSandbox::Destruction::EWorldDestructionTargetDomain::Building;
						Batch.Target.Building = Building;
						Batch.SourceBounds = SourceBounds;
						return UE::ElementSandbox::Destruction::FWorldDestructionAuthorityService::TryConvertResolvedSource(
								*WorldObjects->GetWorld(),
								Batch,
                                [this, Building]()
			{
					return Buildings.IsValid() && Buildings->DestroyEntity(Building);
				},
				[this, Building]()
				{
					return Buildings.IsValid() && Buildings->IsEntityAlive(Building);
				});
		}

		if (!WorldObject.IsSet() || !WorldObjects->IsEntityAlive(WorldObject)
			|| WorldObjects->GetWorldEntityId(WorldObject) != SourceId)
		{
			return true;
		}
		if (!WorldObjects->GetSpatialIndex().TryGetBounds(WorldObject, SourceBounds))
                {
                        return false;
                }
				Batch.Target.Domain = UE::ElementSandbox::Destruction::EWorldDestructionTargetDomain::WorldObject;
				Batch.Target.WorldObject = WorldObject;
				Batch.SourceBounds = SourceBounds;
				return UE::ElementSandbox::Destruction::FWorldDestructionAuthorityService::TryConvertResolvedSource(
						*WorldObjects->GetWorld(),
						Batch,
                        [this, WorldObject]()
			{
				return WorldObjects.IsValid() && WorldObjects->DestroyEntity(WorldObject);
			},
			[this, WorldObject]()
			{
				return WorldObjects.IsValid() && WorldObjects->IsEntityAlive(WorldObject);
			});
	}

	void ProcessPendingBurnoutConversions()
	{
		TArray<FElementTargetKey> Targets;
		PendingBurnoutConversions.GenerateKeyArray(Targets);
		Targets.Sort();
		for (const FElementTargetKey Target : Targets)
		{
			uint64 ProjectionRevision = 0;
			if (!PendingBurnoutConversions.RemoveAndCopyValue(Target, ProjectionRevision))
			{
				continue;
			}
			if (!TryConvertBurnedOutTarget(Target, ProjectionRevision) && Hosts.Contains(Target))
			{
				// 源销毁被拒绝时，共享事务已回滚产品；保持 BurnedOut 并在下个 Authority Step 重试。
				PendingBurnoutConversions.Add(Target, ProjectionRevision);
			}
		}
	}

	void FlushPersistentDirty()
	{
		for (const FElementTargetKey Target : PendingPersistenceDirty)
		{
			FHostRecord* Host = Hosts.Find(Target);
			FPersistentBinding* Binding = FindPersistentBinding(Target);
			if (Host && Binding && !MarkPersistentBindingDirty(*Binding, *Host))
			{
				UE_LOG(LogTemp, Error, TEXT("Element 持久状态标脏失败。"));
			}
		}
		PendingPersistenceDirty.Reset();
	}

	void ProjectCharacter(FHostRecord& Host, const EFireCombustionPhase Phase, const int32 Stack)
	{
		UAbilitySystemComponent* ASC = Characters->ResolveAbilitySystem(Host.Character);
		if (!ASC || !ASC->IsOwnerActorAuthoritative()) return;
		const int32 Desired = Phase == EFireCombustionPhase::Burning ? FMath::Clamp(Stack, 1, 3) : 0;
		FActiveGameplayEffectHandle& Effect = CharacterEffects.FindOrAdd(Host.Target);
		int32 Current = Effect.IsValid() ? ASC->GetCurrentStackCount(Effect) : 0;
		if (Desired == 0)
		{
			if (Effect.IsValid()) ASC->RemoveActiveGameplayEffect(Effect);
			CharacterEffects.Remove(Host.Target);
			return;
		}
		while (Current < Desired)
		{
			Effect = ASC->ApplyGameplayEffectToSelf(
				GetDefault<UElementCharacterBurningEffect>(), 1.0f, ASC->MakeEffectContext());
			if (!Effect.IsValid()) return;
			Current = ASC->GetCurrentStackCount(Effect);
		}
		if (Current > Desired) ASC->RemoveActiveGameplayEffect(Effect, Current - Desired);
	}

	bool ApplyBurnAmount(FHostRecord& Host, const float Amount)
	{
		if (Host.Building.IsSet())
		{
			if (!Buildings->GetRegistry().IsAlive(Host.Building) || Host.BurnCustomDataIndex < 0) return false;
			if (!FElementFireBuildingState::SetBurnCustomData(
				Buildings->GetRegistry(), Host.Building, Host.BurnCustomDataIndex,
				Amount)) return false;
			return Buildings->CommitRenderCustomDataChange(Host.Building);
		}
		return Host.WorldObject.IsSet()
			&& Trees->CommitBurnAmount(Host.WorldObject, Amount);
	}

	double EstimateBurnPresentationTime(
		const FBurnPresentation& Presentation,
		const UWorld& World) const
	{
		return static_cast<double>(Presentation.AuthorityReferenceMilliseconds)
			+ FMath::Max(0.0, static_cast<double>(World.GetTimeSeconds())
				- Presentation.LocalReferenceSeconds) * 1000.0;
	}

	void UpdateBurnPresentation(
		FHostRecord& Host,
		const FElementProjectionCommand& Command,
		const EFireCombustionPhase Phase)
	{
		UWorld* World = Owner.IsValid() ? Owner->GetWorld() : nullptr;
		if (!World) return;
		const int64 BurnStart = FMath::Max<int64>(0, FMath::RoundToInt64(Command.Payload.Values[4]));
		const int64 BurnEnd = FMath::Max<int64>(0, FMath::RoundToInt64(Command.Payload.Values[5]));
		const int64 ReferenceTime = Command.Payload.Count >= 7
			? FMath::Max<int64>(0, FMath::RoundToInt64(Command.Payload.Values[6]))
			: BurnStart;
		if (Phase != EFireCombustionPhase::Burning)
		{
			BurnPresentations.Remove(Host.Target);
			ApplyBurnAmount(Host, ComputeBurnPresentationAmount(
				Phase, BurnStart, BurnEnd, static_cast<double>(ReferenceTime)));
			return;
		}

		FBurnPresentation& Presentation = BurnPresentations.FindOrAdd(Host.Target);
		const bool bSameInterval = Presentation.Phase == Phase
			&& Presentation.BurnStartMilliseconds == BurnStart
			&& Presentation.BurnEndMilliseconds == BurnEnd;
		Presentation.Phase = Phase;
		Presentation.BurnStartMilliseconds = BurnStart;
		Presentation.BurnEndMilliseconds = BurnEnd;
		Presentation.AuthorityReferenceMilliseconds = ReferenceTime;
		Presentation.LocalReferenceSeconds = World->GetTimeSeconds();
		if (!bSameInterval) Presentation.LastAppliedAmount = -1.0f;
		const float Amount = ComputeBurnPresentationAmount(
			Phase, BurnStart, BurnEnd, static_cast<double>(ReferenceTime));
		if ((Presentation.LastAppliedAmount < 0.0f
			|| FMath::Abs(Amount - Presentation.LastAppliedAmount) >= BurnPresentationPrecision)
			&& ApplyBurnAmount(Host, Amount))
		{
			Presentation.LastAppliedAmount = Amount;
		}
	}

	void TickBurnPresentations(UWorld& World)
	{
		for (auto It = BurnPresentations.CreateIterator(); It; ++It)
		{
			FHostRecord* Host = Hosts.Find(It.Key());
			if (!Host)
			{
				It.RemoveCurrent();
				continue;
			}
			FBurnPresentation& Presentation = It.Value();
			const float Amount = ComputeBurnPresentationAmount(
				Presentation.Phase,
				Presentation.BurnStartMilliseconds,
				Presentation.BurnEndMilliseconds,
				EstimateBurnPresentationTime(Presentation, World));
			if (Presentation.LastAppliedAmount >= 0.0f
				&& FMath::Abs(Amount - Presentation.LastAppliedAmount) < BurnPresentationPrecision
				&& Amount < 1.0f)
			{
				continue;
			}
			if (ApplyBurnAmount(*Host, Amount)) Presentation.LastAppliedAmount = Amount;
		}
	}

	void ProjectStick(FHostRecord& Host, const bool bBurning)
	{
		if (UWorldObjectProxyComponent* Proxy = WorldObjects->GetProxy(Host.WorldObject))
		{
			if (IElementFireWorldObjectProjection* Projection =
				Cast<IElementFireWorldObjectProjection>(Proxy->GetOwner()))
			{
				Projection->ApplyElementFireBurning(bBurning);
			}
		}
	}

	void ProjectVisual(
		FHostRecord& Host,
		const FElementProjectionCommand& Command,
		const EFireCombustionPhase Phase)
	{
		if (!Host.WorldEntityId.IsSet() || Host.Profile == EElementFireTargetProfile::Stick
			|| Host.Profile == EElementFireTargetProfile::Character) return;
		const TSharedPtr<FElementVisualJournal, ESPMode::ThreadSafe> Journal = Simulation->GetVisualJournal();
		if (!Journal) return;
		const FElementVisualKey Key = FElementVisualKey::MakePersistent(Host.WorldEntityId, FireVisualKind, 0);
		if (Phase != EFireCombustionPhase::Burning)
		{
			if (const FElementVisualShardKey* Shard = VisualShards.Find(Host.Target))
			{
				Journal->Remove(*Shard, Key, EElementVisualChangeKind::StateEnded, Command.Revision);
				VisualShards.Remove(Host.Target);
			}
			return;
		}
		const FBox Bounds = Host.MakeCompoundShape().CalculateWorldBounds();
		if (Bounds.IsValid == 0) return;
		const FVector FlameScale = Host.WorldObject.IsSet() ? TreeFlameScale : BuildingFlameScale;
		const double HalfHeight = FireConeUnscaledHalfHeightCentimeters * FlameScale.Z;
		const FVector Center(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z + HalfHeight);
		const FElementVisualShardKey Shard = FElementVisualShardKey::FromWorldLocation(Center);
		FElementVisualDescriptor Descriptor;
		Descriptor.Key = Key;
		Descriptor.VisualDefinitionId = FireVisualDefinition;
		Descriptor.Shard = Shard;
		Descriptor.WorldTransform = FTransform(FQuat::Identity, Center, FlameScale);
		Descriptor.WorldBounds = FBox::BuildAABB(Center, FVector(
			FireConeUnscaledHalfHeightCentimeters * FlameScale.X,
			FireConeUnscaledHalfHeightCentimeters * FlameScale.Y,
			HalfHeight));
		Descriptor.Color = FLinearColor(1.0f, 0.25f, 0.02f, 1.0f);
		const FFireCombustionProfile* Profile = SelectProfile(Host.Profile);
		Descriptor.Intensity = Profile ? static_cast<float>(Profile->EmittedFireIntensity) : 1.0f;
		Descriptor.StartTimeMilliseconds = FMath::FloorToInt64(Command.Payload.Values[4]);
		Descriptor.EndTimeMilliseconds = FMath::CeilToInt64(Command.Payload.Values[5]);
		Descriptor.Revision = Command.Revision;
		if (Journal->Upsert(Descriptor) != EElementVisualMutationResult::Rejected)
		{
			VisualShards.Add(Host.Target, Shard);
		}
	}

	void RemoveProjection(FHostRecord& Host, const EElementVisualChangeKind Kind)
	{
		BurnPresentations.Remove(Host.Target);
		if (Host.Profile == EElementFireTargetProfile::Character)
		{
			if (UAbilitySystemComponent* ASC = Characters->ResolveAbilitySystem(Host.Character))
			{
				RemoveAllCharacterBurningEffects(*ASC);
			}
			CharacterEffects.Remove(Host.Target);
		}
		else if (Host.Profile == EElementFireTargetProfile::Stick)
		{
			ProjectStick(Host, false);
		}
		if (const FElementVisualShardKey* Shard = VisualShards.Find(Host.Target))
		{
			if (const TSharedPtr<FElementVisualJournal, ESPMode::ThreadSafe> Journal = Simulation->GetVisualJournal())
			{
				Journal->Remove(
					*Shard,
					FElementVisualKey::MakePersistent(Host.WorldEntityId, FireVisualKind, 0),
					Kind,
					AdvanceNonZero(Host.LastProjectionRevision));
			}
			VisualShards.Remove(Host.Target);
		}
	}

	void OnWorldPostActorTick(UWorld* World, ELevelTick, float)
	{
		if (!bInitialized || !Owner.IsValid() || World != Owner->GetWorld()) return;
		PumpBuildingSnapshotBatches();
		TickBurnPresentations(*World);
		if (Execution) Execution->PumpWorkers(GetAuthorityTime(Storage.Get(), World), false);
	}

	void OnAuthorityStep(const int64 NowMilliseconds)
	{
		if (!bInitialized || !Execution) return;
		ExpireRuntimeSources(NowMilliseconds);
		// 已完成的 Worker 页先在本 Barrier 发布；同一 Barrier 绝不再 Pump 结构命令产生的新 Source。
		if (Execution->CommitAuthorityBarrier(NowMilliseconds))
		{
			ApplyProjectionCommands();
			ApplyStructuralCommands();
			FlushPersistentDirty();
		}
		else if (Execution->PumpWorkers(NowMilliseconds, true)
			&& Execution->CommitAuthorityBarrier(NowMilliseconds))
		{
			ApplyProjectionCommands();
			ApplyStructuralCommands();
			FlushPersistentDirty();
		}
		ProcessPendingBurnoutConversions();
	}

	FElementRuntimeFireSourceHandle CreateFireball(const FVector& WorldLocation)
	{
		if (!Execution || WorldLocation.ContainsNaN() || NextRuntimeSourceId == 0) return {};
		FElementRuntimeFireSourceHandle Handle;
		Handle.Id = NextRuntimeSourceId++;
		Handle.Generation = 1;
		const int64 Now = GetAuthorityTime(Storage.Get(), Owner.IsValid() ? Owner->GetWorld() : nullptr);
		FFireSourceFragment Fragment;
		Fragment.Shape.WorldTransform = FTransform(FQuat::Identity, WorldLocation);
		Fragment.Shape.Shapes.Add(FElementShape::MakeSphere(
			FVector::ZeroVector, Rules.FireballSphereRadius));
		Fragment.Intensity = Rules.Fireball.Intensity;
		Fragment.RangeCentimeters = Rules.Fireball.RangeCentimeters;
		Fragment.Policy = Rules.Fireball.Policy;
		Fragment.ExpireTimeMilliseconds = Now + Rules.FireballLifetimeMilliseconds;
		const FElementEntityHandle Element = Execution->CreateElement();
		if (!Element.IsSet() || !Execution->AddFragment(Element, Fragment))
		{
			if (Element.IsSet()) Execution->DestroyElement(Element);
			return {};
		}
		FRuntimeSource Source;
		Source.Handle = Handle;
		Source.Element = Element;
		Source.ExpireTimeMilliseconds = Fragment.ExpireTimeMilliseconds;
		Source.Token = NextRuntimeSourceToken++;
		RuntimeSources.Add(Handle, Source);
		RuntimeSourceExpiries.HeapPush(
			{Source.ExpireTimeMilliseconds, Handle, Source.Token}, FExpiryPredicate());
		return Handle;
	}

	bool RemoveRuntimeSource(const FElementRuntimeFireSourceHandle Handle)
	{
		FRuntimeSource Source;
		if (!RuntimeSources.RemoveAndCopyValue(Handle, Source) || !Execution) return false;
		return Execution->DestroyElement(Source.Element);
	}

	void ExpireRuntimeSources(const int64 Now)
	{
		while (!RuntimeSourceExpiries.IsEmpty()
			&& RuntimeSourceExpiries.HeapTop().DueTimeMilliseconds <= Now)
		{
			FRuntimeSourceExpiry Expiry;
			RuntimeSourceExpiries.HeapPop(Expiry, FExpiryPredicate(), EAllowShrinking::No);
			const FRuntimeSource* Source = RuntimeSources.Find(Expiry.Handle);
			if (!Source || Source->Token != Expiry.Token) continue;
			RemoveRuntimeSource(Expiry.Handle);
		}
	}

	bool SetStickInteraction(const FWorldEntityId WorldEntityId, const bool bActive)
	{
		if (!Execution || !WorldEntityId.IsSet()) return false;
		const FWorldObjectEntityHandle Entity = WorldObjects->FindEntity(WorldEntityId);
		const FElementTargetKey* Target = WorldObjectTargets.Find(Entity);
		FHostRecord* Host = Target ? Hosts.Find(*Target) : nullptr;
		if (!Host || Host->Profile != EElementFireTargetProfile::Stick) return false;
		if (Host->bFireInteractionActive == bActive) return true;
		Host->bFireInteractionActive = bActive;
		return PublishTarget(
			*Host,
			GetAuthorityTime(Storage.Get(), Owner.IsValid() ? Owner->GetWorld() : nullptr),
			false,
			Host->WorldTransform,
			EElementQueryPriority::Critical);
	}

	bool IsStickInteractionActive(const FWorldEntityId WorldEntityId) const
	{
		if (!WorldObjects.IsValid() || !WorldEntityId.IsSet()) return false;
		const FWorldObjectEntityHandle Entity = WorldObjects->FindEntity(WorldEntityId);
		const FElementTargetKey* Target = WorldObjectTargets.Find(Entity);
		const FHostRecord* Host = Target ? Hosts.Find(*Target) : nullptr;
		return Host && Host->Profile == EElementFireTargetProfile::Stick
			&& Host->bFireInteractionActive;
	}

	static bool ValidateRestoredFireState(
		const FElementAuthorityTargetStateSnapshot& State,
		FString& OutError)
	{
		if (!State.IsValid() || State.StateValues.Num() != 1
			|| State.StateValues[0].SchemaId != ElementFireRuntimeNames::ThermalState
			|| State.StateValues[0].Payload.Count != 8)
		{
			OutError = TEXT("Element Fire Restore 需要且只允许一个版本化 Thermal State。");
			return false;
		}
		for (const FElementNumericValue& Value : State.NumericValues)
		{
			if ((Value.Channel != ElementFireRuntimeNames::ThermalInput
				&& Value.Channel != ElementFireRuntimeNames::CurrentFireRate) || Value.Value < 0.0)
			{
				OutError = TEXT("Element Fire Restore 含未知或非法 Numeric Channel。");
				return false;
			}
		}
		for (const FElementPersistentWake& Wake : State.Wakes)
		{
			if (Wake.ProcessorId != ElementFireRuntimeNames::StateProcessor)
			{
				OutError = TEXT("Element Fire Restore 含未知唤醒 Processor。");
				return false;
			}
		}
		const FElementStateValue& Thermal = State.StateValues[0];
		const double Phase = Thermal.Payload.Values[0];
		const double GasStack = Thermal.Payload.Values[6];
		if (Phase != FMath::RoundToDouble(Phase) || Phase < 0.0
			|| Phase > static_cast<double>(EFireCombustionPhase::BurnedOut)
			|| Thermal.Payload.Values[1] < 0.0 || Thermal.Payload.Values[5] < 0.0
			|| Thermal.Payload.Values[7] < 0.0
			|| GasStack != FMath::RoundToDouble(GasStack) || GasStack < 0.0 || GasStack > 3.0
			|| FMath::RoundToInt64(Thermal.Payload.Values[4]) != State.LastSettlementMilliseconds)
		{
			OutError = TEXT("Element Fire Restore 的 Thermal State 语义非法。");
			return false;
		}
		return true;
	}

	static FElementProjectionCommand MakeProjectionFromState(
		const FElementAuthorityTargetStateSnapshot& State,
		const int64 ReferenceTimeMilliseconds)
	{
		FElementProjectionCommand Command;
		Command.Target = State.Target;
		Command.Channel = ElementFireRuntimeNames::Projection;
		if (State.StateValues.IsEmpty()) return Command;
		const FElementStateValue& Thermal = State.StateValues[0];
		Command.Revision = Thermal.Revision;
		Command.Payload.Count = 7;
		Command.Payload.Values[0] = Thermal.Payload.Values[0];
		Command.Payload.Values[1] = Thermal.Payload.Values[1];
		Command.Payload.Values[2] = Thermal.Payload.Values[7];
		Command.Payload.Values[3] = Thermal.Payload.Values[6];
		Command.Payload.Values[4] = Thermal.Payload.Values[2];
		Command.Payload.Values[5] = Thermal.Payload.Values[3];
		Command.Payload.Values[6] = static_cast<double>(FMath::Max<int64>(
			0, ReferenceTimeMilliseconds));
		return Command;
	}
