// ConfigLoader.cpp
// Implementation of runtime config file loading/saving

#include "ConfigLoader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

FString UConfigLoader::FindConfigFile()
{
    // Priority order:
    // 1. Next to executable (for packaged builds)
    // 2. Project root directory (for editor/development)
    
    FString ExeDir = FPaths::GetPath(FPlatformProcess::ExecutablePath());
    FString ConfigInExeDir = FPaths::Combine(ExeDir, TEXT("GameConfig.ini"));
    
    if (FPaths::FileExists(ConfigInExeDir))
    {
        return ConfigInExeDir;
    }
    
    // For packaged builds, also check parent directories
    // (exe might be in Binaries/Win64/ subfolder)
    FString ParentDir = FPaths::GetPath(ExeDir);
    FString ConfigInParent = FPaths::Combine(ParentDir, TEXT("GameConfig.ini"));
    if (FPaths::FileExists(ConfigInParent))
    {
        return ConfigInParent;
    }
    
    // Check 2 levels up (Cesiumstart/Binaries/Win64 -> Cesiumstart/)
    FString GrandParentDir = FPaths::GetPath(ParentDir);
    FString ConfigInGrandParent = FPaths::Combine(GrandParentDir, TEXT("GameConfig.ini"));
    if (FPaths::FileExists(ConfigInGrandParent))
    {
        return ConfigInGrandParent;
    }
    
    // Fallback to project directory (for editor)
    FString ProjectDir = FPaths::ProjectDir();
    FString ConfigInProject = FPaths::Combine(ProjectDir, TEXT("GameConfig.ini"));
    if (FPaths::FileExists(ConfigInProject))
    {
        return ConfigInProject;
    }
    
    // Return the exe directory path even if file doesn't exist (for creation)
    return ConfigInExeDir;
}

FString UConfigLoader::GetConfigFilePath()
{
    return FindConfigFile();
}

bool UConfigLoader::ConfigFileExists()
{
    return FPaths::FileExists(FindConfigFile());
}

FRuntimeConfig UConfigLoader::LoadConfig()
{
    FRuntimeConfig Config;
    FString ConfigPath = FindConfigFile();

    if (!FPaths::FileExists(ConfigPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("[ConfigLoader] Config file not found at: %s"), *ConfigPath);
        UE_LOG(LogTemp, Warning, TEXT("[ConfigLoader] Using default values. Create GameConfig.ini to customize."));
        return Config;
    }

    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *ConfigPath))
    {
        UE_LOG(LogTemp, Error, TEXT("[ConfigLoader] Failed to read config file: %s"), *ConfigPath);
        return Config;
    }

    // Parse simple key=value format (one per line)
    TArray<FString> Lines;
    FileContent.ParseIntoArrayLines(Lines);

    for (const FString& Line : Lines)
    {
        // Skip empty lines and comments
        FString TrimmedLine = Line.TrimStartAndEnd();
        if (TrimmedLine.IsEmpty() || TrimmedLine.StartsWith(TEXT("#")) || TrimmedLine.StartsWith(TEXT(";")))
        {
            continue;
        }

        FString Key, Value;
        if (TrimmedLine.Split(TEXT("="), &Key, &Value))
        {
            Key = Key.TrimStartAndEnd();
            Value = Value.TrimStartAndEnd();

            // UDP Settings
            if (Key.Equals(TEXT("ListenIP"), ESearchCase::IgnoreCase))
            {
                Config.ListenIP = Value;
            }
            else if (Key.Equals(TEXT("ListenPort"), ESearchCase::IgnoreCase))
            {
                Config.ListenPort = FCString::Atoi(*Value);
            }
            // Cesium Tileset URLs
            else if (Key.Equals(TEXT("BuildingsTilesetURL"), ESearchCase::IgnoreCase))
            {
                Config.BuildingsTilesetURL = Value;
            }
            else if (Key.Equals(TEXT("VegetationTilesetURL"), ESearchCase::IgnoreCase))
            {
                Config.VegetationTilesetURL = Value;
            }
            else if (Key.Equals(TEXT("TerrainTilesetURL"), ESearchCase::IgnoreCase))
            {
                Config.TerrainTilesetURL = Value;
            }
            else if (Key.Equals(TEXT("CustomTileset1URL"), ESearchCase::IgnoreCase))
            {
                Config.CustomTileset1URL = Value;
            }
            else if (Key.Equals(TEXT("CustomTileset2URL"), ESearchCase::IgnoreCase))
            {
                Config.CustomTileset2URL = Value;
            }
            // Target Settings
            else if (Key.Equals(TEXT("TargetActorNamePattern"), ESearchCase::IgnoreCase))
            {
                Config.TargetActorNamePattern = Value;
            }
            // Debug Settings
            else if (Key.Equals(TEXT("EnableDebugLog"), ESearchCase::IgnoreCase))
            {
                Config.bEnableDebugLog = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
            }
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[ConfigLoader] Loaded config from: %s"), *ConfigPath);
    UE_LOG(LogTemp, Log, TEXT("[ConfigLoader] ListenIP=%s, ListenPort=%d"), *Config.ListenIP, Config.ListenPort);
    
    if (!Config.BuildingsTilesetURL.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("[ConfigLoader] BuildingsTilesetURL=%s"), *Config.BuildingsTilesetURL);
    }
    if (!Config.VegetationTilesetURL.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("[ConfigLoader] VegetationTilesetURL=%s"), *Config.VegetationTilesetURL);
    }

    return Config;
}

bool UConfigLoader::SaveConfig(const FRuntimeConfig& Config)
{
    FString ExeDir = FPaths::GetPath(FPlatformProcess::ExecutablePath());
    FString ConfigPath = FPaths::Combine(ExeDir, TEXT("GameConfig.ini"));

    FString Content;
    Content += TEXT("# ============================================\n");
    Content += TEXT("# Cesiumstart Runtime Configuration\n");
    Content += TEXT("# Edit this file to change settings without rebuilding\n");
    Content += TEXT("# ============================================\n\n");

    Content += TEXT("# === UDP Settings ===\n");
    Content += FString::Printf(TEXT("ListenIP=%s\n"), *Config.ListenIP);
    Content += FString::Printf(TEXT("ListenPort=%d\n\n"), Config.ListenPort);

    Content += TEXT("# === Cesium Tileset URLs ===\n");
    Content += TEXT("# Leave empty to use values set in Unreal Editor\n");
    Content += FString::Printf(TEXT("BuildingsTilesetURL=%s\n"), *Config.BuildingsTilesetURL);
    Content += FString::Printf(TEXT("VegetationTilesetURL=%s\n"), *Config.VegetationTilesetURL);
    Content += FString::Printf(TEXT("TerrainTilesetURL=%s\n"), *Config.TerrainTilesetURL);
    Content += FString::Printf(TEXT("CustomTileset1URL=%s\n"), *Config.CustomTileset1URL);
    Content += FString::Printf(TEXT("CustomTileset2URL=%s\n\n"), *Config.CustomTileset2URL);

    Content += TEXT("# === Target Settings ===\n");
    Content += FString::Printf(TEXT("TargetActorNamePattern=%s\n\n"), *Config.TargetActorNamePattern);

    Content += TEXT("# === Debug Settings ===\n");
    Content += FString::Printf(TEXT("EnableDebugLog=%s\n"), Config.bEnableDebugLog ? TEXT("true") : TEXT("false"));

    bool bSuccess = FFileHelper::SaveStringToFile(Content, *ConfigPath);
    
    if (bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("[ConfigLoader] Saved config to: %s"), *ConfigPath);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[ConfigLoader] Failed to save config to: %s"), *ConfigPath);
    }

    return bSuccess;
}

bool UConfigLoader::CreateDefaultConfigFile()
{
    if (ConfigFileExists())
    {
        UE_LOG(LogTemp, Log, TEXT("[ConfigLoader] Config file already exists, not overwriting"));
        return true;
    }

    FRuntimeConfig DefaultConfig;
    return SaveConfig(DefaultConfig);
}
