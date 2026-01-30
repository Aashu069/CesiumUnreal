// CesiumShrinkWrap.h
// ShrinkWrap functionality for conforming meshes to Cesium terrain
// Ported concept from Unity Deora.Tools.ShrinkWrap for Unreal Engine

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "KismetProceduralMeshLibrary.h"
#include "CesiumShrinkWrap.generated.h"

// Raycast direction enum (similar to Unity version)
UENUM(BlueprintType)
enum class EShrinkWrapDirection : uint8
{
	Down UMETA(DisplayName = "Down (-Z)"),
	Up UMETA(DisplayName = "Up (+Z)"),
	Forward UMETA(DisplayName = "Forward (+X)"),
	Back UMETA(DisplayName = "Back (-X)"),
	Right UMETA(DisplayName = "Right (+Y)"),
	Left UMETA(DisplayName = "Left (-Y)")
};

// Configuration for shrink wrap operation
USTRUCT(BlueprintType)
struct FShrinkWrapConfig
{
	GENERATED_BODY()

	// Maximum distance for raycasts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	float RaycastDistance = 10000.0f;  // 100 meters in UE units (cm)

	// Offset from the terrain surface (positive = above terrain)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	float Offset = 50.0f;  // 50cm above terrain

	// Direction to cast rays
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	EShrinkWrapDirection RaycastDirection = EShrinkWrapDirection::Down;

	// Collision channel to trace against (WorldStatic works best for Cesium tiles)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	TEnumAsByte<ECollisionChannel> CollisionChannel = ECC_WorldStatic;

	// If true, trace against complex collision (slower but more accurate for Cesium tiles)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	bool bTraceComplex = true;

	// Update frequency in seconds (0 = manual only, negative = disabled)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	float UpdateFrequency = 0.0f;

	// If true, will also check upward when downward ray misses (for vertices under terrain)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	bool bBidirectionalCheck = true;

	// Delay before first wrap attempt (allows Cesium tiles to load)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	float InitialDelay = 3.0f;  // 3 seconds for tiles to stream in

	// If true, will smooth vertex heights with neighbors to reduce jagged edges
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	bool bSmoothResults = false;

	// Smoothing iterations (only used if bSmoothResults is true)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config", meta = (EditCondition = "bSmoothResults", ClampMin = "1", ClampMax = "10"))
	int32 SmoothingIterations = 2;
};

// Result of a wrap operation
USTRUCT(BlueprintType)
struct FShrinkWrapResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 TotalVertices = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 VerticesWrapped = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	int32 VerticesMissed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Results")
	bool bSuccess = false;
};

// Debug point for visualization
USTRUCT()
struct FWrapDebugPoint
{
	GENERATED_BODY()

	FVector OriginalPosition = FVector::ZeroVector;
	FVector TargetPosition = FVector::ZeroVector;
	bool bHit = false;
};

/**
 * ShrinkWrap Component that can be attached to any actor with a mesh.
 * Wraps the mesh vertices onto terrain surfaces using raycasting.
 * Designed to work with Cesium 3D Tilesets for terrain.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class CESIUMSTART_API UCesiumShrinkWrapComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCesiumShrinkWrapComponent();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

public:
	// ==================== CONFIGURATION ====================
	
	// Shrink wrap configuration
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	FShrinkWrapConfig Config;

	// Target mesh component to wrap (auto-detected if not set)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	UStaticMeshComponent* TargetMeshComponent;

	// Optional: Specific actors to trace against (if empty, traces against all)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	TArray<AActor*> TerrainActors;

	// Enable debug visualization
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap|Debug")
	bool bEnableDebugVisualization = false;

	// Enable verbose logging
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap|Debug")
	bool bEnableVerboseLogging = false;

	// ==================== RUNTIME STATE ====================
	
	// Is the mesh currently wrapped?
	UPROPERTY(BlueprintReadOnly, Category = "Shrink Wrap|State")
	bool bIsWrapped = false;

	// Is a wrap operation in progress?
	UPROPERTY(BlueprintReadOnly, Category = "Shrink Wrap|State")
	bool bIsWrapping = false;

	// ==================== PUBLIC METHODS ====================

	// Perform the wrap operation (main function)
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	FShrinkWrapResult Wrap();

	// Reset to original mesh
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	void ResetWrap();

	// Preview wrap points without applying (for debugging)
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	void PreviewWrap();

	// Initialize/reinitialize the component
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	void Initialize();

	// Get wrap info as string
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	FString GetWrapInfo() const;

private:
	// Original vertex positions (for reset)
	TArray<FVector> OriginalVertices;
	TArray<FVector> OriginalNormals;
	TArray<int32> OriginalIndices;
	TArray<FVector2D> OriginalUVs;
	TArray<FColor> OriginalVertexColors;
	TArray<FProcMeshTangent> OriginalTangents;
	
	// Debug points for visualization
	TArray<FWrapDebugPoint> DebugPoints;

	// Procedural mesh for runtime modification
	UPROPERTY()
	UProceduralMeshComponent* ProceduralMesh;

	// Timer for auto-updates
	float TimeSinceLastUpdate = 0.0f;
	bool bInitialized = false;

	// Get the raycast direction vector
	FVector GetRaycastDirection() const;

	// Perform a raycast for a single vertex
	bool RaycastVertex(const FVector& WorldPosition, const FVector& Direction, FVector& OutHitPoint) const;

	// Extract vertices from static mesh
	bool ExtractMeshData();

	// Apply wrapped vertices to procedural mesh
	void ApplyWrappedVertices(const TArray<FVector>& NewVertices);

	// Draw debug visualization
	void DrawDebugVisualization();

	// Logging helpers
	void Log(const FString& Message, bool bForce = false) const;
	void LogWarning(const FString& Message) const;
	void LogError(const FString& Message) const;
};

/**
 * ShrinkWrap Actor - standalone actor version for easier placement.
 * Can target any mesh actor in the scene.
 */
UCLASS(Blueprintable)
class CESIUMSTART_API ACesiumShrinkWrapActor : public AActor
{
	GENERATED_BODY()

public:
	ACesiumShrinkWrapActor();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

public:
	// ==================== CONFIGURATION ====================
	
	// The source mesh actor to wrap (e.g., your water tileset)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	AActor* SourceMeshActor;

	// Shrink wrap configuration
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	FShrinkWrapConfig Config;

	// Optional: Specific terrain actors to trace against
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	TArray<AActor*> TerrainActors;

	// Enable debug visualization
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap|Debug")
	bool bEnableDebugVisualization = false;

	// Enable verbose logging
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap|Debug")
	bool bEnableVerboseLogging = false;

	// Wrap on begin play?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	bool bWrapOnBeginPlay = true;

	// ==================== RUNTIME STATE ====================
	
	UPROPERTY(BlueprintReadOnly, Category = "Shrink Wrap|State")
	bool bIsWrapped = false;

	UPROPERTY(BlueprintReadOnly, Category = "Shrink Wrap|State")
	bool bIsWrapping = false;

	// ==================== PUBLIC METHODS ====================

	// Main wrap function for all mesh components in the source actor
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	FShrinkWrapResult WrapAllMeshes();

	// Wrap a single static mesh component
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	FShrinkWrapResult WrapMeshComponent(UStaticMeshComponent* MeshComponent);

	// Reset all wrapped meshes
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	void ResetAllWraps();

private:
	// Track created procedural meshes for cleanup
	UPROPERTY()
	TArray<UProceduralMeshComponent*> CreatedProceduralMeshes;

	// Original mesh components that were hidden
	UPROPERTY()
	TArray<UStaticMeshComponent*> HiddenOriginalMeshes;

	// Debug points
	TArray<FWrapDebugPoint> DebugPoints;

	// Timer for auto-updates
	float TimeSinceLastUpdate = 0.0f;

	// Get raycast direction
	FVector GetRaycastDirection() const;

	// Raycast from a position
	bool RaycastVertex(const FVector& WorldPosition, const FVector& Direction, FVector& OutHitPoint) const;

	// Draw debug visualization
	void DrawDebugVisualization();

	// Logging
	void Log(const FString& Message, bool bForce = false) const;
	void LogWarning(const FString& Message) const;
	void LogError(const FString& Message) const;
};
