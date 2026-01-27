// CesiumConfigApplier.h
// Helper actor to apply runtime config to Cesium tilesets

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ConfigLoader.h"
#include "CesiumConfigApplier.generated.h"

/**
 * Actor that applies runtime configuration to Cesium 3D Tilesets
 * Place this in your level and assign your tileset actors
 * It will read GameConfig.ini and update the tileset URLs on BeginPlay
 */
UCLASS(Blueprintable, BlueprintType, meta=(DisplayName="Cesium Config Applier"))
class CESIUMSTART_API ACesiumConfigApplier : public AActor
{
    GENERATED_BODY()

public:
    ACesiumConfigApplier();

protected:
    virtual void BeginPlay() override;

public:
    // ==================== Tileset References ====================
    
    /** Reference to Buildings tileset actor (Cesium3DTileset) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tilesets")
    AActor* BuildingsTileset;

    /** Reference to Vegetation tileset actor */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tilesets")
    AActor* VegetationTileset;

    /** Reference to Terrain tileset actor */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tilesets")
    AActor* TerrainTileset;

    /** Reference to Custom tileset 1 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tilesets")
    AActor* CustomTileset1;

    /** Reference to Custom tileset 2 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tilesets")
    AActor* CustomTileset2;

    // ==================== Settings ====================
    
    /** If true, only apply config if URL is non-empty */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
    bool bOnlyApplyNonEmptyURLs = true;

    /** Enable logging */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
    bool bEnableLogging = true;

    // ==================== Functions ====================
    
    /** Manually reload and apply config (can be called from Blueprint) */
    UFUNCTION(BlueprintCallable, Category = "Config")
    void ReloadAndApplyConfig();

    /** Get the currently loaded config */
    UFUNCTION(BlueprintCallable, Category = "Config")
    FRuntimeConfig GetCurrentConfig() const { return LoadedConfig; }

private:
    void ApplyURLToTileset(AActor* Tileset, const FString& URL, const FString& TilesetName);
    
    FRuntimeConfig LoadedConfig;
};
