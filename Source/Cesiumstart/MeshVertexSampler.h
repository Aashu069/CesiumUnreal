// MeshVertexSampler.h
// Utility class to sample vertices from Cesium tilesets and spawn foliage

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "MeshVertexSampler.generated.h"

// Struct to hold spawn configuration
USTRUCT(BlueprintType)
struct FSpawnConfiguration
{
	GENERATED_BODY()

	// Random seed for reproducible results
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	int32 RandomSeed = 12345;

	// Range around each vertex to spawn (in cm)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	float SpawnRange = 500.0f;

	// Number of instances per vertex (density control)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	int32 InstancesPerVertex = 3;

	// Skip every N vertices to reduce density (1 = use all, 2 = skip half, etc.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	int32 VertexSkipCount = 10;

	// Scale range for spawned instances
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	FVector MinScale = FVector(30.0f, 30.0f, 30.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	FVector MaxScale = FVector(60.0f, 60.0f, 60.0f);

	// Height offset for spawned instances
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn Config")
	float HeightOffset = 0.0f;
};

// Struct to hold results of vertex sampling
USTRUCT(BlueprintType)
struct FVertexSampleResult
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

UCLASS(Blueprintable, BlueprintType)
class CESIUMSTART_API AMeshVertexSampler : public AActor
{
	GENERATED_BODY()
	
public:	
	AMeshVertexSampler();

protected:
	virtual void BeginPlay() override;

public:	
	virtual void Tick(float DeltaTime) override;

	// ============== MAIN FUNCTIONS ==============

	// Main function to sample vertices from tileset and spawn foliage
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	FVertexSampleResult SampleAndSpawnFoliage(
		AActor* SourceTileset,
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		FSpawnConfiguration Config
	);

	// Get all vertices from an actor's static mesh components
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	TArray<FVector> GetVerticesFromActor(AActor* SourceActor, int32 SkipCount = 1);

	// Get vertices from a specific static mesh component
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	TArray<FVector> GetVerticesFromComponent(UStaticMeshComponent* MeshComponent);

	// Spawn instances at given positions with random offset
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	int32 SpawnInstancesAtPositions(
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		const TArray<FVector>& Positions,
		FSpawnConfiguration Config
	);

	// Spawn instances ONLY on a specific actor (filters traces to only hit that actor)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	int32 SpawnInstancesOnActor(
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		const TArray<FVector>& Positions,
		FSpawnConfiguration Config,
		AActor* TargetSurfaceActor
	);

	// ============== CHILD ACTOR HELPERS ==============

	// Get all child actors from a parent actor (works with Cesium 3D Tilesets)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Child Actors")
	TArray<AActor*> GetChildActorsRecursive(AActor* ParentActor, bool bRecursive = true);

	// Get all child actors that have mesh components
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Child Actors")
	TArray<AActor*> GetChildActorsWithMeshes(AActor* ParentActor, bool bRecursive = true);

	// Get all attached actors (different from child actors - includes components attached via sockets)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Child Actors")
	TArray<AActor*> GetAttachedActorsRecursive(AActor* ParentActor, bool bRecursive = true);

	// Sample and spawn from multiple source actors (use with GetAllChildActors result)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	FVertexSampleResult SampleAndSpawnFromActorArray(
		const TArray<AActor*>& SourceActors,
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		FSpawnConfiguration Config
	);

	// Get vertices from multiple actors
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler")
	TArray<FVector> GetVerticesFromActorArray(const TArray<AActor*>& SourceActors, int32 SkipCount = 1);

	// ============== CESIUM/PRIMITIVE COMPONENT HELPERS ==============

	// Get spawn points from ALL primitive components (works with Cesium tilesets!)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Cesium")
	TArray<FVector> GetSpawnPointsFromPrimitiveComponents(AActor* SourceActor, int32 PointsPerComponent = 9);

	// Get count of all primitive components (meshes, Cesium tiles, etc.)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Cesium")
	int32 GetPrimitiveComponentCount(AActor* SourceActor);

	// Sample and spawn directly from an actor's primitive components (BEST FOR CESIUM)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Cesium")
	FVertexSampleResult SampleAndSpawnFromPrimitives(
		AActor* SourceActor,
		UHierarchicalInstancedStaticMeshComponent* TargetHISM,
		FSpawnConfiguration Config
	);

	// Debug: List all component types on an actor
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Cesium")
	TArray<FString> ListAllComponentTypes(AActor* SourceActor);

	// ============== UTILITY FUNCTIONS ==============

	// Get count of mesh components in an actor
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Utility")
	int32 GetMeshComponentCount(AActor* SourceActor);

	// Get total vertex count without extracting all vertices
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Utility")
	int32 GetTotalVertexCount(AActor* SourceActor);

	// Clear all instances from HISM
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Utility")
	void ClearAllInstances(UHierarchicalInstancedStaticMeshComponent* TargetHISM);

	// Get total child actor count (for debugging)
	UFUNCTION(BlueprintCallable, Category = "Mesh Vertex Sampler|Utility")
	int32 CountChildActors(AActor* ParentActor, bool bRecursive = true);

	// ============== DEBUG FUNCTIONS ==============

	// Enable/disable verbose logging
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bEnableVerboseLogging = true;

private:
	// Random stream for seeded randomization
	FRandomStream RandomStream;

	// Helper to log messages
	void LogMessage(const FString& Message, bool bForceLog = false);
	void LogWarning(const FString& Message);
	void LogError(const FString& Message);

	// Generate random offset within range
	FVector GetRandomOffset(float Range);

	// Generate random scale between min and max
	FVector GetRandomScale(const FVector& MinScale, const FVector& MaxScale);

	// Generate random rotation (primarily yaw)
	FRotator GetRandomRotation();
};
