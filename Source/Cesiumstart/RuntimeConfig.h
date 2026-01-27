// RuntimeConfig.h
// Runtime configuration that can be loaded from external config file

#pragma once

#include "CoreMinimal.h"
#include "RuntimeConfig.generated.h"

/**
 * Runtime configuration struct that holds all configurable settings
 * These can be loaded from GameConfig.ini at runtime without recompiling
 */
USTRUCT(BlueprintType)
struct FRuntimeConfig
{
    GENERATED_BODY()

    // ==================== UDP Settings ====================
    
    /** IP address to listen on (0.0.0.0 = all interfaces) */
    UPROPERTY(BlueprintReadWrite, Category = "UDP")
    FString ListenIP = TEXT("0.0.0.0");

    /** Port to listen for UDP packets */
    UPROPERTY(BlueprintReadWrite, Category = "UDP")
    int32 ListenPort = 9999;

    // ==================== Cesium Tileset URLs ====================
    
    /** URL for buildings tileset (leave empty to use editor value) */
    UPROPERTY(BlueprintReadWrite, Category = "Cesium")
    FString BuildingsTilesetURL = TEXT("");

    /** URL for vegetation/foliage tileset (leave empty to use editor value) */
    UPROPERTY(BlueprintReadWrite, Category = "Cesium")
    FString VegetationTilesetURL = TEXT("");

    /** URL for terrain tileset (leave empty to use editor value) */
    UPROPERTY(BlueprintReadWrite, Category = "Cesium")
    FString TerrainTilesetURL = TEXT("");

    /** URL for custom tileset 1 (leave empty to skip) */
    UPROPERTY(BlueprintReadWrite, Category = "Cesium")
    FString CustomTileset1URL = TEXT("");

    /** URL for custom tileset 2 (leave empty to skip) */
    UPROPERTY(BlueprintReadWrite, Category = "Cesium")
    FString CustomTileset2URL = TEXT("");

    // ==================== Target Settings ====================
    
    /** Name pattern to search for target aircraft */
    UPROPERTY(BlueprintReadWrite, Category = "Target")
    FString TargetActorNamePattern = TEXT("F16");

    // ==================== Debug Settings ====================
    
    /** Enable debug logging */
    UPROPERTY(BlueprintReadWrite, Category = "Debug")
    bool bEnableDebugLog = false;
};
