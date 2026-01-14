// CesiumVertexSampler.h
// Direct vertex extraction from Cesium 3D Tilesets - NO line tracing needed!

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "CesiumVertexSampler.generated.h"

// Struct to hold spawn configuration
USTRUCT(BlueprintType)
struct FCesiumSpawnConfig
{
	GENERATED_BODY()

	// Random seed for reproducible results
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	int32 RandomSeed = 12345;

	// Range around each vertex to spawn (in cm)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	float SpawnRange = 50.0f;

	// Number of instances per vertex (density control)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	int32 InstancesPerVertex = 1;

	// Skip every N vertices to reduce density (1 = use all, 10 = use 10%, etc.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	int32 VertexSkipCount = 1;

	// Scale range for spawned instances
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	FVector MinScale = FVector(30.0f, 30.0f, 30.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	FVector MaxScale = FVector(60.0f, 60.0f, 60.0f);

	// Height offset for spawned instances
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	float HeightOffset = 0.0f;
};

// Struct to hold results
USTRUCT(BlueprintType)
struct FCesiumSpawnResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 TotalVerticesFound = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 VerticesUsed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 InstancesSpawned = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 ComponentsProcessed = 0;
};

// Struct to track each streaming target
USTRUCT()
struct FStreamingTarget
{
	GENERATED_BODY()

	UPROPERTY()
	UHierarchicalInstancedStaticMeshComponent* HISM = nullptr;
	
	FCesiumSpawnConfig Config;
	FRandomStream RandomStream;
	int32 InstancesSpawned = 0;
};

UCLASS(Blueprintable, BlueprintType)
class CESIUMSTART_API ACesiumVertexSampler : public AActor
{
	GENERATED_BODY()
	
public:	
	ACesiumVertexSampler();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

public:	
	// ============== STREAMING SPAWN (Procedural as you fly!) ==============

	// Add a foliage type to streaming spawn (can call multiple times for grass, trees, etc.)
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	void AddStreamingTarget(
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		FCesiumSpawnConfig Config
	);

	// Start streaming spawn on a tileset (call AddStreamingTarget first!)
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	void StartStreamingSpawn(AActor* CesiumTileset);

	// Legacy: Start streaming with single target (for backwards compatibility)
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	void StartStreamingSpawnSingle(
		AActor* CesiumTileset,
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		FCesiumSpawnConfig Config
	);

	// Stop streaming spawn
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	void StopStreamingSpawn();

	// Clear all streaming targets
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	void ClearStreamingTargets();

	// Is streaming currently active?
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	bool IsStreamingActive() const { return bStreamingActive; }

	// Get total instances spawned during streaming
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler|Streaming")
	int32 GetTotalStreamedInstances() const { return TotalStreamedInstances; }

	// ============== ONE-SHOT SPAWN ==============

	// Extract vertices directly from Cesium tileset and spawn foliage
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler")
	FCesiumSpawnResult SpawnOnCesiumTileset(
		AActor* CesiumTileset,
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		FCesiumSpawnConfig Config
	);

	// Get all vertex positions from a Cesium tileset (no spawning)
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler")
	TArray<FVector> GetCesiumVertices(AActor* CesiumTileset, int32 SkipCount = 1);

	// Get vertex count without extracting all vertices
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler")
	int32 GetCesiumVertexCount(AActor* CesiumTileset);

	// Clear all instances from HISM
	UFUNCTION(BlueprintCallable, Category = "Cesium Vertex Sampler")
	void ClearAllInstances(UHierarchicalInstancedStaticMeshComponent* TargetHISM);

	// ============== DEBUG ==============

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bEnableLogging = true;

	// ============== PERFORMANCE SETTINGS ==============

	// How often to check for new tiles (seconds) - higher = less CPU usage
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float StreamingCheckInterval = 0.5f;

	// Maximum instances to spawn per frame (0 = unlimited) - prevents frame drops
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "0", ClampMax = "50000"))
	int32 MaxInstancesPerFrame = 5000;

	// Maximum components to process per tick (0 = unlimited) - spreads work across frames
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "0", ClampMax = "100"))
	int32 MaxComponentsPerTick = 10;

private:
	// Random stream for seeded randomization (used for one-shot spawn)
	FRandomStream RandomStream;

	// Streaming state
	bool bStreamingActive = false;
	AActor* StreamingTileset = nullptr;
	float TimeSinceLastCheck = 0.0f;
	int32 TotalStreamedInstances = 0;
	int32 InstancesThisFrame = 0;
	
	// Multiple streaming targets (grass, trees, bushes, etc.)
	TArray<FStreamingTarget> StreamingTargets;
	
	// Pending components to process (for spreading work across frames)
	TArray<UStaticMeshComponent*> PendingComponents;
	
	// Track which component pointers we've already processed
	TSet<UPrimitiveComponent*> ProcessedComponents;

	// Batch spawning helper
	void SpawnBatchForTarget(FStreamingTarget& Target, const TArray<FVector>& Vertices);

	// Logging helpers
	void Log(const FString& Message);
	void LogWarning(const FString& Message);
	void LogError(const FString& Message);

	// Extract vertices from a single primitive component
	TArray<FVector> ExtractVerticesFromComponent(class UPrimitiveComponent* Component);

	// Random helpers
	FVector GetRandomOffset(float Range);
	FVector GetRandomScale(const FVector& MinScale, const FVector& MaxScale);
	FRotator GetRandomRotation();
};
