// CesiumShrinkWrap.h
// ShrinkWrap functionality for conforming meshes to Cesium terrain
// Ported concept from Unity Deora.Tools.ShrinkWrap for Unreal Engine
// Updated to support Cesium 3D Tilesets using ICesiumPrimitive interface

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "ProceduralMeshComponent.h"
#include "KismetProceduralMeshLibrary.h"

// Forward declaration for Cesium interface
class ICesiumPrimitive;

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

	// How to handle vertices that don't hit terrain
	// 0 = Keep original position (may look disconnected)
	// 1 = Use average Z of successfully wrapped vertices (can cause wall artifacts)
	// 2 = Use fallback height offset from original position
	// 3 = Interpolate from nearest neighbors using IDW (RECOMMENDED - prevents walls)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config", meta = (ClampMin = "0", ClampMax = "3"))
	int32 MissedVertexHandling = 3;  // Default to IDW interpolation

	// Fallback height offset for missed vertices (only used if MissedVertexHandling = 2)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config", meta = (EditCondition = "MissedVertexHandling == 2"))
	float FallbackHeightOffset = 0.0f;

	// Maximum distance to search for neighbor vertices when using IDW interpolation (cm)
	// Used when MissedVertexHandling = 3. Larger values = smoother but slower.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	float NeighborSearchRadius = 10000.0f;  // 100 meters

	// Power parameter for IDW interpolation (p=2 is standard inverse distance squared)
	// Higher values give more weight to nearest points
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float IDWPower = 2.0f;

	// Minimum percentage of vertices that must hit terrain (0.0 to 1.0)
	// Below this threshold, the mesh is skipped
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinHitRatio = 0.05f;  // At least 5% must hit

	// If true, skip mesh components with no successful hits (keeps original visible)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config")
	bool bSkipFullyMissedMeshes = true;

	// Auto-retry settings for when terrain isn't loaded yet (e.g., starting in air)
	// If hit ratio is below MinHitRatio, will retry after RetryInterval seconds
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config|Auto Retry")
	bool bAutoRetryOnLowHitRate = true;

	// Maximum number of retry attempts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config|Auto Retry", meta = (EditCondition = "bAutoRetryOnLowHitRate", ClampMin = "1", ClampMax = "20"))
	int32 MaxRetryAttempts = 10;

	// Seconds between retry attempts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config|Auto Retry", meta = (EditCondition = "bAutoRetryOnLowHitRate", ClampMin = "1.0", ClampMax = "30.0"))
	float RetryInterval = 3.0f;

	// Minimum hit ratio required to consider the wrap successful (for retry logic)
	// If below this, will retry. Set higher than MinHitRatio for more aggressive retrying.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap Config|Auto Retry", meta = (EditCondition = "bAutoRetryOnLowHitRate", ClampMin = "0.1", ClampMax = "1.0"))
	float RetryHitRatioThreshold = 0.5f;  // Retry if less than 50% hit
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
	// If this is null at runtime, will try to find by SourceMeshActorName or SourceMeshActorTag
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	AActor* SourceMeshActor;

	// Fallback: Name of the source actor to find if SourceMeshActor is null
	// Use this for packaged builds where direct references might be lost
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	FString SourceMeshActorName;

	// Fallback: Tag to search for if SourceMeshActor and SourceMeshActorName are empty
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shrink Wrap")
	FName SourceMeshActorTag;

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

	// Current retry attempt count
	UPROPERTY(BlueprintReadOnly, Category = "Shrink Wrap|State")
	int32 CurrentRetryAttempt = 0;

	// Last wrap hit ratio (for debugging)
	UPROPERTY(BlueprintReadOnly, Category = "Shrink Wrap|State")
	float LastHitRatio = 0.0f;

	// ==================== PUBLIC METHODS ====================

	// Main wrap function for all mesh components in the source actor
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	FShrinkWrapResult WrapAllMeshes();

	// Wrap a single static mesh component
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	FShrinkWrapResult WrapMeshComponent(UStaticMeshComponent* MeshComponent);

	// Wrap a Cesium primitive component (CesiumGltfPrimitiveComponent)
	// Uses ICesiumPrimitive interface for CPU-safe vertex access (works in packaged builds)
	FShrinkWrapResult WrapCesiumPrimitive(UPrimitiveComponent* Component, ICesiumPrimitive* CesiumPrimitive);

	// Reset all wrapped meshes
	UFUNCTION(BlueprintCallable, Category = "Shrink Wrap")
	void ResetAllWraps();

private:
	// Track created procedural meshes for cleanup (runtime only, not serialized)
	UPROPERTY(Transient)
	TArray<UProceduralMeshComponent*> CreatedProceduralMeshes;

	// Original mesh components that were hidden (runtime only, not serialized)
	UPROPERTY(Transient)
	TArray<UStaticMeshComponent*> HiddenOriginalMeshes;

	// Hidden Cesium primitive components (runtime only, not serialized)
	UPROPERTY(Transient)
	TArray<UPrimitiveComponent*> HiddenCesiumPrimitives;

	// Debug points
	TArray<FWrapDebugPoint> DebugPoints;

	// Timer for auto-updates
	float TimeSinceLastUpdate = 0.0f;

	// Execute wrap with retry logic
	void ExecuteWrapWithRetry();

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
