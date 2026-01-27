// ConfigLoader.h
// Utility class to load/save runtime configuration from external files

#pragma once

#include "CoreMinimal.h"
#include "RuntimeConfig.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ConfigLoader.generated.h"

/**
 * Static utility class for loading and saving runtime configuration
 * Config file is located next to the executable for easy editing
 */
UCLASS()
class CESIUMSTART_API UConfigLoader : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Load configuration from GameConfig.ini
     * Looks for config file next to executable, falls back to project directory
     */
    UFUNCTION(BlueprintCallable, Category = "Config")
    static FRuntimeConfig LoadConfig();

    /**
     * Save configuration to GameConfig.ini
     * Saves next to executable for packaged builds
     */
    UFUNCTION(BlueprintCallable, Category = "Config")
    static bool SaveConfig(const FRuntimeConfig& Config);

    /**
     * Get the full path to the config file
     */
    UFUNCTION(BlueprintCallable, Category = "Config")
    static FString GetConfigFilePath();

    /**
     * Check if config file exists
     */
    UFUNCTION(BlueprintCallable, Category = "Config")
    static bool ConfigFileExists();

    /**
     * Create a default config file if none exists
     */
    UFUNCTION(BlueprintCallable, Category = "Config")
    static bool CreateDefaultConfigFile();

private:
    static FString FindConfigFile();
};
