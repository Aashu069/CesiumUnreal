// CesiumConfigApplier.cpp
// Implementation of Cesium config applier

#include "CesiumConfigApplier.h"

ACesiumConfigApplier::ACesiumConfigApplier()
{
    PrimaryActorTick.bCanEverTick = false;
}

void ACesiumConfigApplier::BeginPlay()
{
    Super::BeginPlay();
    
    // Small delay to ensure Cesium tilesets are initialized
    FTimerHandle TimerHandle;
    GetWorld()->GetTimerManager().SetTimer(TimerHandle, [this]()
    {
        ReloadAndApplyConfig();
    }, 0.5f, false);
}

void ACesiumConfigApplier::ReloadAndApplyConfig()
{
    // Log where we're looking for config
    FString ConfigPath = UConfigLoader::GetConfigFilePath();
    bool bConfigExists = UConfigLoader::ConfigFileExists();
    
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] ========================================"));
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] Config file path: %s"), *ConfigPath);
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] Config file exists: %s"), bConfigExists ? TEXT("YES") : TEXT("NO"));
    
    LoadedConfig = UConfigLoader::LoadConfig();
    
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] BuildingsTilesetURL from config: %s"), *LoadedConfig.BuildingsTilesetURL);
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] VegetationTilesetURL from config: %s"), *LoadedConfig.VegetationTilesetURL);
    
    // Check tileset references
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] BuildingsTileset assigned: %s"), BuildingsTileset ? TEXT("YES") : TEXT("NO - ASSIGN IN EDITOR!"));
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] VegetationTileset assigned: %s"), VegetationTileset ? TEXT("YES") : TEXT("NO"));
    UE_LOG(LogTemp, Warning, TEXT("[CesiumConfigApplier] ========================================"));
    
    // Apply URLs to tilesets
    ApplyURLToTileset(BuildingsTileset, LoadedConfig.BuildingsTilesetURL, TEXT("Buildings"));
    ApplyURLToTileset(VegetationTileset, LoadedConfig.VegetationTilesetURL, TEXT("Vegetation"));
    ApplyURLToTileset(TerrainTileset, LoadedConfig.TerrainTilesetURL, TEXT("Terrain"));
    ApplyURLToTileset(CustomTileset1, LoadedConfig.CustomTileset1URL, TEXT("Custom1"));
    ApplyURLToTileset(CustomTileset2, LoadedConfig.CustomTileset2URL, TEXT("Custom2"));
}

void ACesiumConfigApplier::ApplyURLToTileset(AActor* Tileset, const FString& URL, const FString& TilesetName)
{
    if (!Tileset)
    {
        return;
    }
    
    // Skip empty URLs if configured to do so
    if (bOnlyApplyNonEmptyURLs && URL.IsEmpty())
    {
        if (bEnableLogging)
        {
            UE_LOG(LogTemp, Log, TEXT("[CesiumConfigApplier] %s: Skipping (empty URL in config, using editor value)"), *TilesetName);
        }
        return;
    }
    
    // Use reflection to call Cesium functions without including headers
    // This avoids linker issues with Cesium's STL dependencies
    
    UClass* TilesetClass = Tileset->GetClass();
    
    // Find and call SetTilesetSource (set to FromUrl = 1)
    UFunction* SetSourceFunc = TilesetClass->FindFunctionByName(TEXT("SetTilesetSource"));
    if (SetSourceFunc)
    {
        // ETilesetSource::FromUrl = 1
        struct { uint8 Source; } Params;
        Params.Source = 1; // FromUrl
        Tileset->ProcessEvent(SetSourceFunc, &Params);
    }
    
    // Find and set the Url property directly
    FProperty* UrlProperty = TilesetClass->FindPropertyByName(TEXT("Url"));
    if (UrlProperty)
    {
        FStrProperty* StrProp = CastField<FStrProperty>(UrlProperty);
        if (StrProp)
        {
            StrProp->SetPropertyValue_InContainer(Tileset, URL);
        }
    }
    
    // Find and call RefreshTileset
    UFunction* RefreshFunc = TilesetClass->FindFunctionByName(TEXT("RefreshTileset"));
    if (RefreshFunc)
    {
        Tileset->ProcessEvent(RefreshFunc, nullptr);
    }
    
    if (bEnableLogging)
    {
        UE_LOG(LogTemp, Log, TEXT("[CesiumConfigApplier] %s: URL set to %s"), *TilesetName, *URL);
    }
}
