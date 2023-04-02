#include "FortInventory.h"
#include "FortPlayerController.h"
#include "FortPickup.h"
#include "FortQuickBars.h"

UFortItem* CreateItemInstance(AFortPlayerController* PlayerController, UFortItemDefinition* ItemDefinition, int Count)
{
	UFortItem* NewItemInstance = ItemDefinition->CreateTemporaryItemInstanceBP(Count);

	if (NewItemInstance && PlayerController)
		NewItemInstance->SetOwningControllerForTemporaryItem(PlayerController);

	return NewItemInstance;
}

std::pair<std::vector<UFortItem*>, std::vector<UFortItem*>> AFortInventory::AddItem(UFortItemDefinition* ItemDefinition, bool* bShouldUpdate, int Count, int LoadedAmmo, bool bShouldAddToStateValues)
{
	if (!ItemDefinition)
		return std::pair<std::vector<UFortItem*>, std::vector<UFortItem*>>();

	if (bShouldUpdate)
		*bShouldUpdate = false;

	if (LoadedAmmo == -1)
	{
		if (auto WeaponDef = Cast<UFortWeaponItemDefinition>(ItemDefinition))
			LoadedAmmo = WeaponDef->GetClipSize();
		else
			LoadedAmmo = 0;
	}

	auto& ItemInstances = GetItemList().GetItemInstances();

	auto MaxStackSize = ItemDefinition->GetMaxStackSize();

	bool bAllowMultipleStacks = ItemDefinition->DoesAllowMultipleStacks();
	int OverStack = 0;

	std::vector<UFortItem*> NewItemInstances;
	std::vector<UFortItem*> ModifiedItemInstances;

	if (MaxStackSize > 1)
	{
		for (int i = 0; i < ItemInstances.Num(); i++)
		{
			auto CurrentItemInstance = ItemInstances.at(i);
			auto CurrentEntry = CurrentItemInstance->GetItemEntry();

			if (CurrentEntry->GetItemDefinition() == ItemDefinition)
			{
				if (CurrentEntry->GetCount() < MaxStackSize || !bAllowMultipleStacks)
				{
					OverStack = CurrentEntry->GetCount() + Count - MaxStackSize;

					if (!bAllowMultipleStacks && !(CurrentEntry->GetCount() < MaxStackSize))
					{
						break;
					}

					int AmountToStack = OverStack > 0 ? Count - OverStack : Count;

					auto ReplicatedEntry = FindReplicatedEntry(CurrentEntry->GetItemGuid());

					CurrentEntry->GetCount() += AmountToStack;
					ReplicatedEntry->GetCount() += AmountToStack;

					// std::cout << std::format("{} : {} : {}\n", CurrentEntry.Count, ReplicatedEntry->Count, OverStack);

					/* if (bAddToStateValues)
					{
						FFortItemEntryStateValue StateValue;
						StateValue.IntValue = 1;
						StateValue.StateType = EFortItemEntryState::ShouldShowItemToast;

						CurrentEntry.StateValues.Add(StateValue);
						ReplicatedEntry->StateValues.Add(StateValue);
					}
					else
					{
						// CurrentEntry.StateValues.FreeBAD();
						// ReplicatedEntry->StateValues.FreeBAD();
					} */

					ModifiedItemInstances.push_back(CurrentItemInstance);

					GetItemList().MarkItemDirty(CurrentEntry);
					GetItemList().MarkItemDirty(ReplicatedEntry);

					if (OverStack <= 0)
						return std::make_pair(NewItemInstances, ModifiedItemInstances);

					// break;
				}
			}
		}
	}

	Count = OverStack > 0 ? OverStack : Count;

	auto PlayerController = Cast<APlayerController>(GetOwner());

	if (!PlayerController)
		return std::make_pair(NewItemInstances, ModifiedItemInstances);

	if (OverStack > 0 && !bAllowMultipleStacks)
	{
		auto Pawn = PlayerController->GetPawn();

		if (!Pawn)
			return std::make_pair(NewItemInstances, ModifiedItemInstances);

		AFortPickup::SpawnPickup(ItemDefinition, Pawn->GetActorLocation(), Count, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, -1, Cast<AFortPawn>(Pawn));
		return std::make_pair(NewItemInstances, ModifiedItemInstances);
	}

	auto FortPlayerController = Cast<AFortPlayerController>(PlayerController);
	UFortItem* NewItemInstance = CreateItemInstance(FortPlayerController, ItemDefinition, Count);

	if (NewItemInstance)
	{
		// if (LoadedAmmo != -1)
		NewItemInstance->GetItemEntry()->GetLoadedAmmo() = LoadedAmmo;

		NewItemInstances.push_back(NewItemInstance);

		// NewItemInstance->GetItemEntry()->GetItemDefinition() = ItemDefinition;

		static auto FortItemEntryStruct = FindObject(L"/Script/FortniteGame.FortItemEntry");
		static auto FortItemEntrySize = *(int*)(__int64(FortItemEntryStruct) + Offsets::PropertiesSize);

		// LOG_INFO(LogDev, "FortItemEntryStruct {}", __int64(FortItemEntryStruct));
		// LOG_INFO(LogDev, "FortItemEntrySize {}", __int64(FortItemEntrySize));

		ItemInstances.Add(NewItemInstance);
		GetItemList().GetReplicatedEntries().Add(*NewItemInstance->GetItemEntry(), FortItemEntrySize);

		if (FortPlayerController && Engine_Version < 420)
		{
			static auto QuickBarsOffset = FortPlayerController->GetOffset("QuickBars", false);
			auto QuickBars = FortPlayerController->Get<AActor*>(QuickBarsOffset);

			if (QuickBars)
			{
				struct
				{
					FGuid                                       Item;                                                     // (Parm, IsPlainOldData)
					EFortQuickBars                                     InQuickBar;                                               // (Parm, ZeroConstructor, IsPlainOldData)
					int                                                Slot;                                                     // (Parm, ZeroConstructor, IsPlainOldData)
				}
				AFortQuickBars_ServerAddItemInternal_Params
				{
					NewItemInstance->GetItemEntry()->GetItemGuid(),
					IsPrimaryQuickbar(ItemDefinition) ? EFortQuickBars::Primary : EFortQuickBars::Secondary,
					-1
				};

				static auto ServerAddItemInternalFn = FindObject<UFunction>("/Script/FortniteGame.FortQuickBars.ServerAddItemInternal");
				QuickBars->ProcessEvent(ServerAddItemInternalFn, &AFortQuickBars_ServerAddItemInternal_Params);
			}
		}

		if (bShouldUpdate)
			*bShouldUpdate = true;
	}

	return std::make_pair(NewItemInstances, ModifiedItemInstances);
}

bool AFortInventory::RemoveItem(const FGuid& ItemGuid, bool* bShouldUpdate, int Count, bool bForceRemoval)
{
	if (bShouldUpdate)
		*bShouldUpdate = false;

	auto ItemInstance = FindItemInstance(ItemGuid);
	auto ReplicatedEntry = FindReplicatedEntry(ItemGuid);

	if (!ItemInstance || !ReplicatedEntry)
		return false;

	auto ItemDefinition = Cast<UFortWorldItemDefinition>(ReplicatedEntry->GetItemDefinition());

	if (!ItemDefinition)
		return false;

	if (Count < 0)
	{
		Count = 0;
		bForceRemoval = true;
	}

	auto NewCount = ReplicatedEntry->GetCount() - Count;

	auto& ItemInstances = GetItemList().GetItemInstances();
	auto& ReplicatedEntries = GetItemList().GetReplicatedEntries();

	bool bOverrideChangeStackSize = false;

	if (ItemDefinition->ShouldPersistWhenFinalStackEmpty() && !bForceRemoval)
	{
		bool bIsFinalStack = true;

		for (int i = 0; i < ItemInstances.Num(); i++)
		{
			auto ItemInstance = ItemInstances.at(i);

			if (ItemInstance->GetItemEntry()->GetItemDefinition() == ItemDefinition && ItemInstance->GetItemEntry()->GetItemGuid() != ItemGuid)
			{
				bIsFinalStack = false;
				break;
			}
		}

		if (bIsFinalStack)
		{
			NewCount = NewCount < 0 ? 0 : NewCount; // min(NewCount, 0) or something i forgot
			bOverrideChangeStackSize = true;
		}
	}

	if (NewCount > 0 || bOverrideChangeStackSize)
	{
		ItemInstance->GetItemEntry()->GetCount() = NewCount;
		ReplicatedEntry->GetCount() = NewCount;

		GetItemList().MarkItemDirty(ItemInstance->GetItemEntry());
		GetItemList().MarkItemDirty(ReplicatedEntry);

		return true;
	}

	static auto FortItemEntryStruct = FindObject<UStruct>(L"/Script/FortniteGame.FortItemEntry");
	static auto FortItemEntrySize = FortItemEntryStruct->GetPropertiesSize();

	for (int i = 0; i < ItemInstances.Num(); i++)
	{
		if (ItemInstances.at(i)->GetItemEntry()->GetItemGuid() == ItemGuid)
		{
			ItemInstances.Remove(i);
			break;
		}
	}

	for (int i = 0; i < ReplicatedEntries.Num(); i++)
	{
		if (ReplicatedEntries.at(i, FortItemEntrySize).GetItemGuid() == ItemGuid)
		{
			ReplicatedEntries.Remove(i, FortItemEntrySize);
			break;
		}
	}

	auto FortPlayerController = Cast<AFortPlayerController>(GetOwner());

	if (FortPlayerController && Engine_Version < 420)
	{
		static auto QuickBarsOffset = FortPlayerController->GetOffset("QuickBars", false);
		auto QuickBars = FortPlayerController->Get<AFortQuickBars*>(QuickBarsOffset);

		if (QuickBars)
		{
			auto SlotIndex = QuickBars->GetSlotIndex(ItemGuid);

			if (SlotIndex != -1)
			{
				QuickBars->ServerRemoveItemInternal(ItemGuid, false, true);
				QuickBars->EmptySlot(IsPrimaryQuickbar(ItemDefinition) ? EFortQuickBars::Primary : EFortQuickBars::Secondary, SlotIndex);
			}
		}
	}

	// todo remove from weaponlist

	if (bShouldUpdate)
		*bShouldUpdate = true;

	return true;
}

void AFortInventory::ModifyCount(UFortItem* ItemInstance, int New, bool bRemove, std::pair<FFortItemEntry*, FFortItemEntry*>* outEntries, bool bUpdate)
{
	auto ReplicatedEntry = FindReplicatedEntry(ItemInstance->GetItemEntry()->GetItemGuid());

	if (!ReplicatedEntry)
		return;

	if (!bRemove)
	{
		ItemInstance->GetItemEntry()->GetCount() += New;
		ReplicatedEntry->GetCount() += New;
	}
	else
	{
		ItemInstance->GetItemEntry()->GetCount() -= New;
		ReplicatedEntry->GetCount() -= New;
	}

	if (outEntries)
		*outEntries = { ItemInstance->GetItemEntry(), ReplicatedEntry};

	if (bUpdate || !outEntries)
	{
		GetItemList().MarkItemDirty(ItemInstance->GetItemEntry());
		GetItemList().MarkItemDirty(ReplicatedEntry);
	}
}

UFortItem* AFortInventory::GetPickaxeInstance()
{
	static auto FortWeaponMeleeItemDefinitionClass = FindObject<UClass>("/Script/FortniteGame.FortWeaponMeleeItemDefinition");

	auto& ItemInstances = GetItemList().GetItemInstances();

	for (int i = 0; i < ItemInstances.Num(); i++)
	{
		auto ItemInstance = ItemInstances.At(i);

		if (ItemInstance->GetItemEntry()->GetItemDefinition()->IsA(FortWeaponMeleeItemDefinitionClass))
			return ItemInstance;
	}
	
	return nullptr;
}

UFortItem* AFortInventory::FindItemInstance(UFortItemDefinition* ItemDefinition)
{
	auto& ItemInstances = GetItemList().GetItemInstances();

	for (int i = 0; i < ItemInstances.Num(); i++)
	{
		auto ItemInstance = ItemInstances.At(i);

		if (ItemInstance->GetItemEntry()->GetItemDefinition() == ItemDefinition)
			return ItemInstance;
	}

	return nullptr;
}

void AFortInventory::CorrectLoadedAmmo(const FGuid& Guid, int NewAmmoCount)
{
	auto CurrentWeaponInstance = FindItemInstance(Guid);

	if (!CurrentWeaponInstance)
		return;

	auto CurrentWeaponReplicatedEntry = FindReplicatedEntry(Guid);

	if (!CurrentWeaponReplicatedEntry)
		return;

	if (CurrentWeaponReplicatedEntry->GetLoadedAmmo() != NewAmmoCount)
	{
		CurrentWeaponInstance->GetItemEntry()->GetLoadedAmmo() = NewAmmoCount;
		CurrentWeaponReplicatedEntry->GetLoadedAmmo() = NewAmmoCount;

		GetItemList().MarkItemDirty(CurrentWeaponInstance->GetItemEntry());
		GetItemList().MarkItemDirty(CurrentWeaponReplicatedEntry);
	}
}

UFortItem* AFortInventory::FindItemInstance(const FGuid& Guid)
{
	auto& ItemInstances = GetItemList().GetItemInstances();

	for (int i = 0; i < ItemInstances.Num(); i++)
	{
		auto ItemInstance = ItemInstances.At(i);

		if (ItemInstance->GetItemEntry()->GetItemGuid() == Guid)
			return ItemInstance;
	}

	return nullptr;
}

FFortItemEntry* AFortInventory::FindReplicatedEntry(const FGuid& Guid)
{
	static auto FortItemEntryStruct = FindObject<UStruct>(L"/Script/FortniteGame.FortItemEntry");
	static auto FortItemEntrySize = FortItemEntryStruct->GetPropertiesSize();

	auto& ReplicatedEntries = GetItemList().GetReplicatedEntries();

	for (int i = 0; i < ReplicatedEntries.Num(); i++)
	{
		auto& ReplicatedEntry = ReplicatedEntries.At(i, FortItemEntrySize);

		if (ReplicatedEntry.GetItemGuid() == Guid)
			return &ReplicatedEntry;
	}

	return nullptr;
}