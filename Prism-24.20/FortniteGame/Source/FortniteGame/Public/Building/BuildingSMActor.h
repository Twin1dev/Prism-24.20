#pragma once

#include "CoreGlobals.h"
#include <Templates/Casts.h>

PRISM_NAMESPACE_START

class ABuildingSMActor : public UE_EXTENDS_SDK_CLASS(ABuildingSMActor)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(ABuildingSMActor);

	void AttemptSpawnResources(const FBuildingSMActorAttemptSpawnResourcesParams& Params);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(AttemptSpawnResources);

	void SetEditingPlayer(AFortPlayerStateZone* NewEditingPlayer)
	{
		if (Role == ENetRole::ROLE_Authority && (!EditingPlayer || !NewEditingPlayer))
		{
			SetNetDormancy(NewEditingPlayer ? ENetDormancy::DORM_DormantAll : ENetDormancy::DORM_Awake);
			ForceNetUpdate();

			if (EditingPlayer)
			{
				if (AFortPlayerController* PlayerController = Cast<AFortPlayerController>(EditingPlayer->Owner))
				{
					EditingPlayer = NewEditingPlayer;
					OnRep_EditingPlayer();
					return;
				}
			}
			else
			{
				if (!NewEditingPlayer)
				{
					EditingPlayer = NewEditingPlayer;
					OnRep_EditingPlayer();
					return;
				}

				if (AFortPlayerController* PlayerController = Cast<AFortPlayerController>(NewEditingPlayer->Owner))
				{
					EditingPlayer = NewEditingPlayer;
					OnRep_EditingPlayer();
				}
			}
		}
	}

	ABuildingSMActor* ReplaceBuildingActor(EBuildingReplacementType ReplacementType, TSubclassOf<ABuildingSMActor> ReplacementClass, int32 ReplacementUpgradeLevel, int32 RotationIterations, bool InbMirrored, AFortPlayerController* EditingController)
	{
		static ABuildingSMActor* (*Function)(ABuildingSMActor*, EBuildingReplacementType, TSubclassOf<ABuildingSMActor>, int32, int32, bool, AFortPlayerController*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x9CBB934);

		return Function(this, ReplacementType, ReplacementClass, ReplacementUpgradeLevel, RotationIterations, InbMirrored, EditingController);
	}

	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(ABuildingSMActor);
UE_DECLARE_PRISM_TYPE(ABuildingSMActor);

PRISM_NAMESPACE_END