#include "CesiumTileMaterialSwapperComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "StaticMeshResources.h"
#include "TimerManager.h"

namespace {
// Lazy initialization to avoid static FName crash during module load
static const TArray<FName>& GetCandidateVectorParams() {
    static TArray<FName> Params = {
        FName(TEXT("BaseColorFactor")),
        FName(TEXT("baseColorFactor")),
        FName(TEXT("BaseColor")),
        FName(TEXT("Color")),
        FName(TEXT("Tint")),
        FName(TEXT("Albedo")),
        FName(TEXT("AlbedoColor")),
        FName(TEXT("BaseColorTint")),
    };
    return Params;
}

static FLinearColor HexSRGB(const TCHAR* Hex) {
  FLinearColor Linear;
  if (UCesiumTileMaterialSwapperComponent::HexToLinearColor(FString(Hex), Linear)) {
    return Linear;
  }
  return FLinearColor::Black;
}
} // namespace

UCesiumTileMaterialSwapperComponent::UCesiumTileMaterialSwapperComponent() {
  PrimaryComponentTick.bCanEverTick = false;

  // Defaults from the provided SHADER_COLORS snippet.
  WallColors = {
      HexSRGB(TEXT("#FF00FF")),
      HexSRGB(TEXT("#FF00AA")),
      HexSRGB(TEXT("#FF0055")),
      HexSRGB(TEXT("#AA00FF")),
  };

  // Roof colors: combine flat/gabled/hip into one "roof" palette.
  RoofColors = {
      HexSRGB(TEXT("#084A1C")),
      HexSRGB(TEXT("#0A6A2C")),
      HexSRGB(TEXT("#0C8A3C")),
      HexSRGB(TEXT("#FFCC00")),
      HexSRGB(TEXT("#FFDD22")),
      HexSRGB(TEXT("#FFEE44")),
      HexSRGB(TEXT("#8800FF")),
      HexSRGB(TEXT("#9922FF")),
      HexSRGB(TEXT("#AA44FF")),
  };

  WindowColors = {
      HexSRGB(TEXT("#00FF00")),
  };
}

void UCesiumTileMaterialSwapperComponent::BeginPlay() {
  Super::BeginPlay();

  if (!TilesetActor) {
    // Allow attaching this component directly to the tileset actor.
    TilesetActor = GetOwner();
  }

  if (!GetWorld()) {
    return;
  }

  if (bLogSwapper) {
    UE_LOG(LogTemp, Log, TEXT("[CesiumMaterialSwapper] BeginPlay. TilesetActor=%s, Interval=%.2fs"),
           TilesetActor ? *TilesetActor->GetName() : TEXT("<null>"), ScanIntervalSeconds);
    UE_LOG(LogTemp, Log, TEXT("[CesiumMaterialSwapper] Materials set? Wall=%s Roof=%s Window=%s"),
           WallMaterial ? *WallMaterial->GetName() : TEXT("<null>"),
           RoofMaterial ? *RoofMaterial->GetName() : TEXT("<null>"),
           WindowMaterial ? *WindowMaterial->GetName() : TEXT("<null>"));

      UE_LOG(LogTemp, Log, TEXT("[CesiumMaterialSwapper] VertexColorClassifierMaterial=%s"),
        VertexColorClassifierMaterial ? *VertexColorClassifierMaterial->GetName() : TEXT("<null>"));
  }

  GetWorld()->GetTimerManager().SetTimer(
      ScanTimer, this, &UCesiumTileMaterialSwapperComponent::ScanAndSwap, ScanIntervalSeconds, true);
}

void UCesiumTileMaterialSwapperComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  if (GetWorld()) {
    GetWorld()->GetTimerManager().ClearTimer(ScanTimer);
  }
  Super::EndPlay(EndPlayReason);
}

void UCesiumTileMaterialSwapperComponent::ScanAndSwap() {
  if (!TilesetActor) {
    return;
  }

  RemainingMaterialLogsThisTick = (bLogMaterialParameters && MaxLoggedMaterialsPerTick > 0) ? MaxLoggedMaterialsPerTick : 0;

  // Process the tileset actor itself.
  ProcessActor(TilesetActor);

  // Also process attached child actors (Cesium can spawn tile actors/components under the tileset).
  TArray<AActor*> AttachedActors;
  TilesetActor->GetAttachedActors(AttachedActors);
  for (AActor* Attached : AttachedActors) {
    ProcessActor(Attached);
  }
}

bool UCesiumTileMaterialSwapperComponent::MeshHasVertexColors(UMeshComponent* MeshComponent) {
  const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(MeshComponent);
  if (!StaticMeshComponent) {
    return false;
  }

  const UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
  if (!StaticMesh) {
    return false;
  }

  const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
  if (!RenderData || RenderData->LODResources.Num() == 0) {
    return false;
  }

  const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
  return LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0;
}

void UCesiumTileMaterialSwapperComponent::ProcessActor(AActor* ActorToProcess) {
  if (!ActorToProcess) {
    return;
  }

  TArray<UMeshComponent*> MeshComponents;
  ActorToProcess->GetComponents<UMeshComponent>(MeshComponents);

  if (bLogSwapper) {
    UE_LOG(LogTemp, Verbose, TEXT("[CesiumMaterialSwapper] Scanning Actor=%s MeshComponents=%d"),
           *ActorToProcess->GetName(), MeshComponents.Num());
  }

  for (UMeshComponent* MeshComponent : MeshComponents) {
    ProcessMeshComponent(MeshComponent);
  }
}

void UCesiumTileMaterialSwapperComponent::ProcessMeshComponent(UMeshComponent* MeshComponent) {
  if (!MeshComponent) {
    return;
  }

  if (bOnlyProcessEachComponentOnce && ProcessedComponents.Contains(MeshComponent)) {
    return;
  }

  const int32 NumMats = MeshComponent->GetNumMaterials();
  if (NumMats <= 0) {
    return;
  }

  // If the dataset encodes semantic colors in vertex colors (COLOR_0), the material parameters
  // will not contain the neon colors (baseColorFactor usually stays at 1,1,1). In that case the
  // correct approach is a single material that reads VertexColor and chooses textures accordingly.
  if (VertexColorClassifierMaterial) {
    for (int32 MaterialIndex = 0; MaterialIndex < NumMats; ++MaterialIndex) {
      ApplySwap(MeshComponent, MaterialIndex, VertexColorClassifierMaterial);
    }

    if (bOnlyProcessEachComponentOnce) {
      ProcessedComponents.Add(MeshComponent);
    }
    return;
  }

  // Helpful diagnostic when the user expects color-based swapping.
  if (bLogSwapper && MeshHasVertexColors(MeshComponent)) {
    UE_LOG(LogTemp, Warning,
           TEXT("[CesiumMaterialSwapper] Mesh=%s has vertex colors (COLOR_0). baseColorFactor matching will not work; set VertexColorClassifierMaterial."),
           *MeshComponent->GetName());
  }

  if (bLogSwapper) {
    UE_LOG(LogTemp, Verbose, TEXT("[CesiumMaterialSwapper] Mesh=%s NumMaterials=%d"), *MeshComponent->GetName(), NumMats);
  }

  for (int32 MaterialIndex = 0; MaterialIndex < NumMats; ++MaterialIndex) {
    UMaterialInterface* CurrentMaterial = MeshComponent->GetMaterial(MaterialIndex);
    if (!CurrentMaterial) {
      continue;
    }

    if (bLogMaterialParameters) {
      const bool bWithinBudget = (MaxLoggedMaterialsPerTick <= 0) || (RemainingMaterialLogsThisTick > 0);
      UMaterialInstance* MI = Cast<UMaterialInstance>(CurrentMaterial);
      if (bWithinBudget && MI) {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Guids;
        MI->GetAllVectorParameterInfo(Infos, Guids);
        UE_LOG(LogTemp, Log, TEXT("[CesiumMaterialSwapper] Material[%d]=%s VectorParams=%d"),
               MaterialIndex, *CurrentMaterial->GetName(), Infos.Num());
        for (const FMaterialParameterInfo& Info : Infos) {
          FLinearColor V;
          if (MI->GetVectorParameterValue(Info, V)) {
            UE_LOG(LogTemp, Log, TEXT("  - %s = (%.3f %.3f %.3f %.3f)"), *Info.Name.ToString(), V.R, V.G, V.B, V.A);
          } else {
            UE_LOG(LogTemp, Log, TEXT("  - %s = <no value>"), *Info.Name.ToString());
          }
        }

        if (MaxLoggedMaterialsPerTick > 0) {
          --RemainingMaterialLogsThisTick;
        }
      } else if (bWithinBudget) {
        UE_LOG(LogTemp, Log, TEXT("[CesiumMaterialSwapper] Material[%d]=%s (not a MaterialInstance)"),
               MaterialIndex, *CurrentMaterial->GetName());
        if (MaxLoggedMaterialsPerTick > 0) {
          --RemainingMaterialLogsThisTick;
        }
      }
    }

    // 1) Try color-based classification.
    FLinearColor BaseColor;
    const bool bHasBaseColor = TryGetBaseColorLinear(CurrentMaterial, BaseColor);
    if (bHasBaseColor) {
      if (WallMaterial && TryMatchAny(BaseColor, WallColors, ColorTolerance)) {
        ApplySwap(MeshComponent, MaterialIndex, WallMaterial);
        continue;
      }
      if (RoofMaterial && TryMatchAny(BaseColor, RoofColors, ColorTolerance)) {
        ApplySwap(MeshComponent, MaterialIndex, RoofMaterial);
        continue;
      }
      if (WindowMaterial && TryMatchAny(BaseColor, WindowColors, ColorTolerance)) {
        ApplySwap(MeshComponent, MaterialIndex, WindowMaterial);
        continue;
      }
    }

    // 2) Optional name-based fallback.
    if (bFallbackToNameMatch) {
      const FString Name = CurrentMaterial->GetName().ToLower();
      if (WallMaterial && Name.Contains(TEXT("wall"))) {
        ApplySwap(MeshComponent, MaterialIndex, WallMaterial);
        continue;
      }
      if (RoofMaterial && Name.Contains(TEXT("roof"))) {
        ApplySwap(MeshComponent, MaterialIndex, RoofMaterial);
        continue;
      }
      if (WindowMaterial && Name.Contains(TEXT("window"))) {
        ApplySwap(MeshComponent, MaterialIndex, WindowMaterial);
        continue;
      }
    }
  }

  if (bOnlyProcessEachComponentOnce) {
    ProcessedComponents.Add(MeshComponent);
  }
}

void UCesiumTileMaterialSwapperComponent::ApplySwap(
    UMeshComponent* MeshComponent,
    int32 MaterialIndex,
    UMaterialInterface* Replacement) {
  if (!MeshComponent || !Replacement) {
    return;
  }

  UMaterialInterface* Current = MeshComponent->GetMaterial(MaterialIndex);
  if (Current == Replacement) {
    return;
  }

  MeshComponent->SetMaterial(MaterialIndex, Replacement);

  if (bLogSwapper) {
    UE_LOG(LogTemp, Log, TEXT("[CesiumMaterialSwapper] Swap: Mesh=%s Slot=%d %s -> %s"),
           *MeshComponent->GetName(), MaterialIndex,
           Current ? *Current->GetName() : TEXT("<null>"),
           *Replacement->GetName());
  }
}

bool UCesiumTileMaterialSwapperComponent::TryGetBaseColorLinear(
    UMaterialInterface* Material,
    FLinearColor& OutBaseColor) {
  if (!Material) {
    return false;
  }

  UMaterialInstance* MI = Cast<UMaterialInstance>(Material);
  if (!MI) {
    return false;
  }

  // Try common parameter names used by glTF/Cesium materials.
  for (const FName& ParamName : GetCandidateVectorParams()) {
    FLinearColor Value;

#if ENGINE_MAJOR_VERSION >= 5
    // UE5: UMaterialInstance has GetVectorParameterValue(FMaterialParameterInfo,...)
    const FMaterialParameterInfo Info(ParamName);
    if (MI->GetVectorParameterValue(Info, Value)) {
      OutBaseColor = Value;
      return true;
    }
#else
    // Older UE: fall back to MID Blueprint-friendly access if possible.
    if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(MI)) {
      Value = MID->K2_GetVectorParameterValue(ParamName);
      OutBaseColor = Value;
      return true;
    }
#endif

    if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(MI)) {
      // If the parameter doesn't exist, Unreal returns (0,0,0,0). We guard by checking if the
      // parameter is actually present via the MI path above. This is a best-effort fallback.
      Value = MID->K2_GetVectorParameterValue(ParamName);
      // Heuristic: if it's non-zero, treat it as valid.
      if (Value.A != 0.0f || Value.R != 0.0f || Value.G != 0.0f || Value.B != 0.0f) {
        OutBaseColor = Value;
        return true;
      }
    }
  }

  // If Cesium used a different parameter name, try to locate something that looks like base color.
  {
    TArray<FMaterialParameterInfo> Infos;
    TArray<FGuid> Guids;
    MI->GetAllVectorParameterInfo(Infos, Guids);
    for (const FMaterialParameterInfo& Info : Infos) {
      const FString Lower = Info.Name.ToString().ToLower();
      if (!Lower.Contains(TEXT("base")) && !Lower.Contains(TEXT("color")) && !Lower.Contains(TEXT("albedo")) && !Lower.Contains(TEXT("tint"))) {
        continue;
      }
      FLinearColor Value;
      if (MI->GetVectorParameterValue(Info, Value)) {
        OutBaseColor = Value;
        return true;
      }
    }
  }

  return false;
}

bool UCesiumTileMaterialSwapperComponent::TryMatchAny(
    const FLinearColor& Color,
    const TArray<FLinearColor>& Palette,
    float Tolerance) {
  const FVector3f C(Color.R, Color.G, Color.B);

  for (const FLinearColor& P : Palette) {
    const FVector3f Pc(P.R, P.G, P.B);
    const float Dist = (C - Pc).Size();
    if (Dist <= Tolerance) {
      return true;
    }
  }

  return false;
}

bool UCesiumTileMaterialSwapperComponent::HexToLinearColor(const FString& Hex, FLinearColor& OutLinear) {
  FString S = Hex;
  S.TrimStartAndEndInline();

  if (S.StartsWith(TEXT("#"))) {
    S.RightChopInline(1);
  }

  if (S.Len() != 6 && S.Len() != 8) {
    return false;
  }

  // UE 5.7: use FColor::FromHex (expects RRGGBB or RRGGBBAA).
  if (S.Len() == 6) {
    S.Append(TEXT("FF"));
  }

  const FColor SRGB = FColor::FromHex(S);
  OutLinear = FLinearColor::FromSRGBColor(SRGB);
  return true;
}
