// CesiumVertexSampler.cpp
// Direct vertex extraction from Cesium 3D Tilesets - NO line tracing!
// Uses standard UE APIs only - CesiumGltfPrimitiveComponent IS a UStaticMeshComponent!

#include "CesiumVertexSampler.h"
#include "CesiumGeoreference.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"

// Windows SEH for catching Cesium buffer access violations
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// SEH helper function - must be separate because __try can't be used with C++ objects
// Returns true if copy succeeded, false if access violation occurred
static bool SafeMemcpyWithSEH(void* Dest, const void* Src, SIZE_T Size)
{
	__try
	{
		FMemory::Memcpy(Dest, Src, Size);
		return true;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		// Access violation - Cesium freed the buffer during copy
		return false;
	}
}

// SEH helper to read a single vertex - returns false if access violation
static bool SafeReadVertexPosition(const FPositionVertexBuffer& Buffer, uint32 Index, FVector3f& OutPos)
{
	__try
	{
		OutPos = Buffer.VertexPosition(Index);
		return true;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

ACesiumVertexSampler::ACesiumVertexSampler()
{
	PrimaryActorTick.bCanEverTick = true; // Enable tick for streaming
}

void ACesiumVertexSampler::BeginPlay()
{
	Super::BeginPlay();
	
	// Cache the CesiumGeoreference for coordinate conversion
	// In packaged builds, tile transforms may be in ECEF, not Unreal coordinates
	CachedGeoreference = ACesiumGeoreference::GetDefaultGeoreference(GetWorld());
	if (CachedGeoreference)
	{
		Log(TEXT("CesiumVertexSampler: Found CesiumGeoreference for coordinate conversion"));
	}
	else
	{
		LogWarning(TEXT("CesiumVertexSampler: NO CesiumGeoreference found!"));
	}
	
	Log(TEXT("CesiumVertexSampler initialized - OPTIMIZED vertex extraction (batch spawning, frame limiting)"));
}

void ACesiumVertexSampler::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bStreamingActive || StreamingTargets.Num() == 0)
	{
		return;
	}

	// Clean up invalid HISM references to prevent use-after-free
	StreamingTargets.RemoveAll([](const FStreamingTarget& T)
	{
		return !IsValid(T.HISM);
	});
	
	if (StreamingTargets.Num() == 0)
	{
		bStreamingActive = false;
		return;
	}
	
	// CRITICAL: Clean up stale pointers from tracking sets/maps
	// Cesium can destroy components at any time - we must not keep dangling pointers
	PendingComponents.RemoveAll([](const UStaticMeshComponent* Comp) { return !IsValid(Comp); });
	
	// Clean up ProcessedComponents - remove destroyed components so we don't hold dangling pointers
	TArray<UPrimitiveComponent*> StaleProcessed;
	for (UPrimitiveComponent* Comp : ProcessedComponents)
	{
		if (!IsValid(Comp))
		{
			StaleProcessed.Add(Comp);
		}
	}
	for (UPrimitiveComponent* Comp : StaleProcessed)
	{
		ProcessedComponents.Remove(Comp);
	}
	
	// Clean up ComponentRetryCounts separately (uses TWeakObjectPtr<UStaticMeshComponent>)
	TArray<TWeakObjectPtr<UStaticMeshComponent>> StaleRetryKeys;
	for (auto& Pair : ComponentRetryCounts)
	{
		if (!Pair.Key.IsValid())
		{
			StaleRetryKeys.Add(Pair.Key);
		}
	}
	for (auto& Key : StaleRetryKeys)
	{
		ComponentRetryCounts.Remove(Key);
	}
	
	// Clean up LastObservedTransforms - uses TWeakObjectPtr so check IsValid
	TArray<TWeakObjectPtr<UStaticMeshComponent>> StaleTransformKeys;
	for (auto& Pair : LastObservedTransforms)
	{
		if (!Pair.Key.IsValid())
		{
			StaleTransformKeys.Add(Pair.Key);
		}
	}
	for (auto& Key : StaleTransformKeys)
	{
		LastObservedTransforms.Remove(Key);
	}

	// Reset per-frame counter
	InstancesThisFrame = 0;

	TimeSinceLastCheck += DeltaTime;

	if (TimeSinceLastCheck < StreamingCheckInterval)
	{
		return;
	}

	TimeSinceLastCheck = 0.0f;

	// Check for new tiles and add to pending queue
	if (StreamingTileset)
	{
		TArray<UStaticMeshComponent*> MeshComponents;
		StreamingTileset->GetComponents<UStaticMeshComponent>(MeshComponents);

		// Find new components and add to pending queue
		for (UStaticMeshComponent* MeshComp : MeshComponents)
		{
			// CRITICAL: Use IsValid() - Cesium can destroy components leaving dangling pointers
			if (!IsValid(MeshComp) || !MeshComp->IsVisible())
			{
				continue;
			}

			// Skip if we already processed this component successfully
			if (ProcessedComponents.Contains(MeshComp))
			{
				continue;
			}

			// Queue for processing (do NOT mark processed yet - wait for successful extraction)
			if (!PendingComponents.Contains(MeshComp))
			{
				PendingComponents.Add(MeshComp);
			}
		}

		// Process limited number of components per tick
		int32 ComponentsToProcess = (MaxComponentsPerTick > 0) 
			? FMath::Min(PendingComponents.Num(), MaxComponentsPerTick) 
			: PendingComponents.Num();

		int32 NewComponentsProcessed = 0;
		int32 NewInstancesThisTick = 0;

		for (int32 c = 0; c < ComponentsToProcess; c++)
		{
			if (PendingComponents.Num() == 0) break;
			
			// Check frame limit
			if (MaxInstancesPerFrame > 0 && InstancesThisFrame >= MaxInstancesPerFrame)
			{
				Log(FString::Printf(TEXT("Frame limit reached (%d), deferring %d components to next tick"), 
					MaxInstancesPerFrame, PendingComponents.Num()));
				break;
			}

			UStaticMeshComponent* MeshComp = PendingComponents[0];
			PendingComponents.RemoveAt(0);

			// CRITICAL: Use IsValid() instead of null check - Cesium can destroy components
			// at any time, leaving dangling pointers. IsValid() checks the UObject is alive.
			if (!IsValid(MeshComp))
			{
				continue;
			}
			
			// Additional visibility check (safe now that we know object is valid)
			if (!MeshComp->IsVisible())
			{
				continue;
			}
			
			// ========================================================================
			// TRANSFORM STABILITY CHECK: Ensure tile is not still being positioned
			// Cesium tiles can have unstable transforms during loading - wait for stability
			// ========================================================================
			FTransform CurrentTransform = MeshComp->GetComponentTransform();
			
			// Skip if transform is invalid
			if (!CurrentTransform.IsValid() || CurrentTransform.GetScale3D().IsNearlyZero())
			{
				// Re-queue for retry
				PendingComponents.Add(MeshComp);
				continue;
			}
			
			TWeakObjectPtr<UStaticMeshComponent> WeakComp(MeshComp);
			FTransform* LastTransform = LastObservedTransforms.Find(WeakComp);
			
			if (LastTransform)
			{
				// Check if transform has changed since last observation
				bool bTransformStable = 
					FVector::DistSquared(CurrentTransform.GetLocation(), LastTransform->GetLocation()) < 1.0 &&
					CurrentTransform.GetRotation().Equals(LastTransform->GetRotation(), 0.001f) &&
					FVector::DistSquared(CurrentTransform.GetScale3D(), LastTransform->GetScale3D()) < 0.001;
				
				if (!bTransformStable)
				{
					// Transform still changing - update last observed and re-queue
					LastObservedTransforms.Add(WeakComp, CurrentTransform);
					PendingComponents.Add(MeshComp);
					continue;
				}
				
				// Transform is stable - proceed with extraction
				LastObservedTransforms.Remove(WeakComp);
			}
			else
			{
				// First time seeing this component - record transform and wait one more tick
				LastObservedTransforms.Add(WeakComp, CurrentTransform);
				PendingComponents.Add(MeshComp);
				continue;
			}

			// Extract vertices once per component
			TArray<FVector> CompVertices = ExtractVerticesFromComponent(MeshComp);
			
			UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] Tick: Component extraction returned %d vertices"), CompVertices.Num());

			// If extraction failed (tile not ready), re-queue with retry limit
			if (CompVertices.Num() == 0)
			{
				// Track retry count to prevent infinite requeue
				int32& RetryCount = ComponentRetryCounts.FindOrAdd(MeshComp);
				RetryCount++;
				
				if (RetryCount > 10)  // Hard cap - give up after 10 attempts
				{
					ProcessedComponents.Add(MeshComp); // Mark as processed (give up permanently)
					ComponentRetryCounts.Remove(MeshComp);
					continue;
				}
				
				// Re-add to pending for retry on next tick
				PendingComponents.Add(MeshComp);
				continue;
			}

			// Only mark processed after SUCCESSFUL extraction
			ComponentRetryCounts.Remove(MeshComp);  // Clear retry count on success
			ProcessedComponents.Add(MeshComp);
			NewComponentsProcessed++;

			// Spawn on ALL streaming targets using BATCH method
			for (FStreamingTarget& Target : StreamingTargets)
			{
				if (!Target.HISM) continue;

				int32 InstancesBefore = Target.InstancesSpawned;
				SpawnBatchForTarget(Target, CompVertices);
				NewInstancesThisTick += (Target.InstancesSpawned - InstancesBefore);
			}
		}

		if (NewComponentsProcessed > 0)
		{
			Log(FString::Printf(TEXT("Streaming: +%d tiles, +%d instances, %d pending (total: %d)"), 
				NewComponentsProcessed, NewInstancesThisTick, PendingComponents.Num(), TotalStreamedInstances));
		}
	}
}

// OPTIMIZED: Batch spawn all instances at once instead of one-by-one
void ACesiumVertexSampler::SpawnBatchForTarget(FStreamingTarget& Target, const TArray<FVector>& Vertices)
{
	if (!IsValid(Target.HISM) || Vertices.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] SpawnBatchForTarget: Skipping - HISM valid=%d, Vertices=%d"),
			IsValid(Target.HISM), Vertices.Num());
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] SpawnBatchForTarget: Starting with %d vertices for HISM"), Vertices.Num());
	
	// ========================================================================
	// DEBUG: Log player/camera position to understand coordinate system
	// Get player location from multiple sources for robustness in packaged builds
	// ========================================================================
	FVector CameraLocation = FVector::ZeroVector;
	bool bFoundPlayer = false;
	
	if (UWorld* World = GetWorld())
	{
		// Try 1: Get from PlayerController's Pawn
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				CameraLocation = Pawn->GetActorLocation();
				bFoundPlayer = true;
			}
			// Try 2: Get from PlayerController's camera
			else if (PC->PlayerCameraManager)
			{
				CameraLocation = PC->PlayerCameraManager->GetCameraLocation();
				bFoundPlayer = true;
			}
			// Try 3: Get from PlayerController itself
			else
			{
				CameraLocation = PC->GetFocalLocation();
				bFoundPlayer = true;
			}
		}
		
		// Try 4: Iterate all player controllers
		if (!bFoundPlayer)
		{
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* PC = It->Get())
				{
					if (APawn* Pawn = PC->GetPawn())
					{
						CameraLocation = Pawn->GetActorLocation();
						bFoundPlayer = true;
						break;
					}
				}
			}
		}
	}
	
	// Log what we found
	if (bFoundPlayer)
	{
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] Player/Camera Location: (%.1f, %.1f, %.1f)"),
			CameraLocation.X, CameraLocation.Y, CameraLocation.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] Could not find player! Using (0,0,0) - distance filtering disabled"));
	}
	
	// ========================================================================
	// CESIUM COORDINATE FIX: Account for World Origin Rebasing
	// Cesium uses ECEF coordinates (millions of meters). UE handles this with
	// origin rebasing - the world origin shifts as you move. We need to convert
	// our "absolute" world positions to positions relative to the current origin.
	// ========================================================================
	
	// Get the current world origin offset (used by Cesium for origin rebasing)
	FIntVector WorldOrigin = GetWorld()->OriginLocation;
	FVector OriginOffset = FVector(WorldOrigin.X, WorldOrigin.Y, WorldOrigin.Z);
	
	UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] World Origin Offset: (%.1f, %.1f, %.1f)"), 
		OriginOffset.X, OriginOffset.Y, OriginOffset.Z);
	
	// ========================================================================
	// POSITION VALIDATION: Calculate vertex cloud centroid and bounds
	// This helps detect garbage data (trees in sky) by checking coherence
	// ========================================================================
	
	FVector VertexSum = FVector::ZeroVector;
	FVector MinBounds(TNumericLimits<double>::Max());
	FVector MaxBounds(TNumericLimits<double>::Lowest());
	int32 ValidVertexCount = 0;
	
	for (const FVector& V : Vertices)
	{
		if (V.ContainsNaN() || !FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y) || !FMath::IsFinite(V.Z))
		{
			continue;
		}
		// Apply origin offset for bounds calculation
		FVector AdjustedV = V - OriginOffset;
		VertexSum += AdjustedV;
		MinBounds = MinBounds.ComponentMin(AdjustedV);
		MaxBounds = MaxBounds.ComponentMax(AdjustedV);
		ValidVertexCount++;
	}
	
	if (ValidVertexCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] SpawnBatch: All %d vertices were invalid!"), Vertices.Num());
		return;
	}
	
	const FVector Centroid = VertexSum / ValidVertexCount;
	const FVector BoundsExtent = (MaxBounds - MinBounds) * 0.5;
	const double BoundsRadius = BoundsExtent.Size();
	
	// Calculate distance from player to tile centroid (helps verify coordinate conversion)
	FVector DistanceToPlayer = Centroid - CameraLocation;
	double DistMagnitude = DistanceToPlayer.Size();
	
	// Log bounds for debugging
	UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] Vertex cloud: Centroid=(%.1f, %.1f, %.1f), Radius=%.1f, DistToPlayer=%.1f, ValidCount=%d/%d"),
		Centroid.X, Centroid.Y, Centroid.Z, BoundsRadius, DistMagnitude, ValidVertexCount, Vertices.Num());
	
	// SANITY CHECK: Only filter clearly garbage data
	// Cesium tiles can span large areas (especially at lower LODs)
	// Only filter if tile radius is absurdly huge (suggests corrupted/garbage vertices)
	// 50,000,000 = 500km which is extremely generous
	static constexpr double MaxReasonableTileRadius = 50000000.0;
	
	if (BoundsRadius > MaxReasonableTileRadius)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] SpawnBatch: Tile radius too large (%.1f > %.1f) - likely corrupted data, skipping"), 
			BoundsRadius, MaxReasonableTileRadius);
		return;
	}
	
	// NOTE: Removed distance-to-player check. Cesium coordinates can legitimately be
	// millions of units from origin. The important thing is that tiles are rendered
	// correctly, which Cesium handles. We just spawn instances at the same positions.

	// SAFETY: Always clamp SkipCount locally to prevent infinite loop
	int32 SafeSkip = FMath::Max(1, Target.Config.VertexSkipCount);
	
	// Pre-calculate how many transforms we'll create
	int32 VerticesUsed = (Vertices.Num() + SafeSkip - 1) / SafeSkip;
	int32 TotalTransforms = VerticesUsed * Target.Config.InstancesPerVertex;

	// Check frame limit
	if (MaxInstancesPerFrame > 0)
	{
		int32 RemainingBudget = MaxInstancesPerFrame - InstancesThisFrame;
		if (RemainingBudget <= 0) return;
		TotalTransforms = FMath::Min(TotalTransforms, RemainingBudget);
	}

	// Pre-allocate transform array for batch adding
	TArray<FTransform> BatchTransforms;
	BatchTransforms.Reserve(TotalTransforms);

	int32 AddedCount = 0;
	int32 SkippedOutliers = 0;
	int32 SkippedTooFarFromPlayer = 0;
	
	// CRITICAL: Filter vertices by distance from PLAYER, not centroid
	// This prevents spawning trees at garbage coordinates (millions of units away)
	// In packaged builds, some tiles have a mix of valid and invalid vertex positions
	// NOTE: If player wasn't found, we disable this filter (set to very large value)
	const double MaxDistanceFromPlayer = bFoundPlayer ? 5000000.0 : 1e15; // 50km if player found, else disabled

	for (int32 i = 0; i < Vertices.Num() && AddedCount < TotalTransforms; i += SafeSkip)
	{
		const FVector& VertexPos = Vertices[i];  // This is in absolute WORLD space
		
		// Skip invalid positions
		if (VertexPos.ContainsNaN() || !FMath::IsFinite(VertexPos.X) || 
			!FMath::IsFinite(VertexPos.Y) || !FMath::IsFinite(VertexPos.Z))
		{
			SkippedOutliers++;
			continue;
		}
		
		// Apply origin offset to get position relative to current world origin
		FVector RebasedVertexPos = VertexPos - OriginOffset;
		
		// CRITICAL: Skip vertices that are too far from the PLAYER (if player was found)
		// This filters out garbage vertices that appear at millions of units in packaged builds
		if (bFoundPlayer)
		{
			const double DistFromPlayer = FVector::Dist(RebasedVertexPos, CameraLocation);
			if (DistFromPlayer > MaxDistanceFromPlayer)
			{
				SkippedTooFarFromPlayer++;
				continue;
			}
		}

		for (int32 j = 0; j < Target.Config.InstancesPerVertex && AddedCount < TotalTransforms; j++)
		{
			// Use target's own random stream for consistent results per foliage type
			float RangeX = Target.RandomStream.FRandRange(-Target.Config.SpawnRange, Target.Config.SpawnRange);
			float RangeY = Target.RandomStream.FRandRange(-Target.Config.SpawnRange, Target.Config.SpawnRange);
			float RangeZ = Target.RandomStream.FRandRange(-Target.Config.SpawnRange * 0.1f, Target.Config.SpawnRange * 0.1f);
			
			// Use REBASED position (relative to current world origin, not absolute ECEF)
			FVector SpawnPos = RebasedVertexPos + FVector(RangeX, RangeY, RangeZ);
			SpawnPos.Z += Target.Config.HeightOffset;

			float Yaw = Target.RandomStream.FRandRange(0.0f, 360.0f);
			FRotator Rotation(0.0f, Yaw, 0.0f);
			
			FVector Scale;
			Scale.X = Target.RandomStream.FRandRange(Target.Config.MinScale.X, Target.Config.MaxScale.X);
			Scale.Y = Target.RandomStream.FRandRange(Target.Config.MinScale.Y, Target.Config.MaxScale.Y);
			Scale.Z = Target.RandomStream.FRandRange(Target.Config.MinScale.Z, Target.Config.MaxScale.Z);

			FTransform InstanceTransform;
			InstanceTransform.SetLocation(SpawnPos);
			InstanceTransform.SetRotation(Rotation.Quaternion());
			InstanceTransform.SetScale3D(Scale);

			BatchTransforms.Add(InstanceTransform);
			AddedCount++;
		}
	}
	
	// Log filtering stats
	if (SkippedOutliers > 0 || SkippedTooFarFromPlayer > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] SpawnBatch: Skipped %d invalid + %d too far from player (max %.0f)"), 
			SkippedOutliers, SkippedTooFarFromPlayer, MaxDistanceFromPlayer);
	}

	// BATCH ADD - Much faster than adding one by one!
	if (BatchTransforms.Num() > 0)
	{
		// Log sample position for debugging
		FVector SamplePos = BatchTransforms[0].GetLocation();
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] SpawnBatch: Adding %d instances. Sample pos: (%.1f, %.1f, %.1f)"),
			BatchTransforms.Num(), SamplePos.X, SamplePos.Y, SamplePos.Z);
		
		// ========================================================================
		// SIMPLE FIX: Use world-space transforms directly
		// The vertices are already in the correct world coordinate space (same as player).
		// No need to move HISM or convert to local - just add in world space.
		// ========================================================================
		
		// Log distance from player to help debug visibility
		FVector DistToPlayer = SamplePos - CameraLocation;
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] Distance from player to spawn: %.1f units (%.1f meters)"),
			DistToPlayer.Size(), DistToPlayer.Size() / 100.0f);
		
		// Use bWorldSpace=TRUE - instances are in world coordinates
		Target.HISM->AddInstances(BatchTransforms, false, true);
		
		// CRITICAL: Force full update of the HISM rendering state
		Target.HISM->BuildTreeIfOutdated(true, false);
		Target.HISM->MarkRenderStateDirty();
		
		Target.InstancesSpawned += BatchTransforms.Num();
		TotalStreamedInstances += BatchTransforms.Num();
		InstancesThisFrame += BatchTransforms.Num();
		
		// VERIFY: Check actual HISM instance count after add
		int32 ActualCount = Target.HISM->GetInstanceCount();
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] SpawnBatch DONE: Total instances now = %d (HISM reports: %d)"), TotalStreamedInstances, ActualCount);
		
		// Log HISM bounds to verify instances are at expected location
		FBoxSphereBounds HISMBounds = Target.HISM->Bounds;
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] HISM Bounds Center: (%.1f, %.1f, %.1f), Radius: %.1f)"),
			HISMBounds.Origin.X, HISMBounds.Origin.Y, HISMBounds.Origin.Z, HISMBounds.SphereRadius);
		
		// Log HISM world location
		FVector HISMActualLoc = Target.HISM->GetComponentLocation();
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] HISM World Location: (%.1f, %.1f, %.1f)"),
			HISMActualLoc.X, HISMActualLoc.Y, HISMActualLoc.Z);
		
		// Warn if mismatch
		if (ActualCount != Target.InstancesSpawned)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] MISMATCH! Expected %d instances but HISM has %d"), Target.InstancesSpawned, ActualCount);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] SpawnBatch: No transforms generated from %d vertices (skipped %d outliers)!"), Vertices.Num(), SkippedOutliers);
	}
}

// ============== STREAMING FUNCTIONS ==============

void ACesiumVertexSampler::AddStreamingTarget(
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	FCesiumSpawnConfig Config)
{
	if (!TargetHISM)
	{
		LogError(TEXT("AddStreamingTarget: TargetHISM is NULL!"));
		return;
	}

	// DIAGNOSTIC: Check HISM state
	UStaticMesh* Mesh = TargetHISM->GetStaticMesh();
	UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] HISM DIAGNOSTICS:"));
	UE_LOG(LogTemp, Warning, TEXT("  - HISM Name: %s"), *TargetHISM->GetName());
	UE_LOG(LogTemp, Warning, TEXT("  - Has Mesh: %s"), Mesh ? TEXT("YES") : TEXT("NO - WILL BE INVISIBLE!"));
	if (Mesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("  - Mesh Name: %s"), *Mesh->GetName());
		UE_LOG(LogTemp, Warning, TEXT("  - Mesh Bounds: %s"), *Mesh->GetBounds().ToString());
	}
	UE_LOG(LogTemp, Warning, TEXT("  - Visible: %s"), TargetHISM->IsVisible() ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogTemp, Warning, TEXT("  - Registered: %s"), TargetHISM->IsRegistered() ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogTemp, Warning, TEXT("  - Component Location: %s"), *TargetHISM->GetComponentLocation().ToString());
	UE_LOG(LogTemp, Warning, TEXT("  - Current Instance Count: %d"), TargetHISM->GetInstanceCount());

	FStreamingTarget NewTarget;
	NewTarget.HISM = TargetHISM;
	NewTarget.Config = Config;
	NewTarget.Config.VertexSkipCount = FMath::Max(1, NewTarget.Config.VertexSkipCount);
	NewTarget.RandomStream.Initialize(Config.RandomSeed);
	NewTarget.InstancesSpawned = 0;

	StreamingTargets.Add(NewTarget);

	Log(FString::Printf(TEXT("Added streaming target: %s (Seed: %d, Skip: %d) - Total targets: %d"), 
		*TargetHISM->GetName(), Config.RandomSeed, Config.VertexSkipCount, StreamingTargets.Num()));
}

void ACesiumVertexSampler::StartStreamingSpawn(AActor* CesiumTileset)
{
	if (!CesiumTileset)
	{
		LogError(TEXT("StartStreamingSpawn: CesiumTileset is NULL!"));
		return;
	}

	if (StreamingTargets.Num() == 0)
	{
		LogError(TEXT("StartStreamingSpawn: No targets added! Call AddStreamingTarget first."));
		return;
	}

	StreamingTileset = CesiumTileset;

	// Clear tracking
	ProcessedComponents.Empty();
	PendingComponents.Empty();
	TotalStreamedInstances = 0;
	InstancesThisFrame = 0;
	TimeSinceLastCheck = StreamingCheckInterval; // Trigger immediate check

	bStreamingActive = true;

	Log(TEXT("========================================"));
	Log(FString::Printf(TEXT("OPTIMIZED STREAMING SPAWN STARTED with %d foliage types!"), StreamingTargets.Num()));
	Log(FString::Printf(TEXT("Settings: CheckInterval=%.2fs, MaxPerFrame=%d, MaxComponentsPerTick=%d"), 
		StreamingCheckInterval, MaxInstancesPerFrame, MaxComponentsPerTick));
	Log(TEXT("Fly around - foliage will spawn on new tiles automatically!"));
}

// Legacy function for backwards compatibility
void ACesiumVertexSampler::StartStreamingSpawnSingle(
	AActor* CesiumTileset,
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	FCesiumSpawnConfig Config)
{
	if (!CesiumTileset)
	{
		LogError(TEXT("StartStreamingSpawnSingle: CesiumTileset is NULL!"));
		return;
	}

	if (!TargetHISM)
	{
		LogError(TEXT("StartStreamingSpawnSingle: TargetHISM is NULL!"));
		return;
	}

	// Clear existing targets and add this one
	StreamingTargets.Empty();
	AddStreamingTarget(TargetHISM, Config);
	StartStreamingSpawn(CesiumTileset);
}

void ACesiumVertexSampler::ClearStreamingTargets()
{
	StreamingTargets.Empty();
	Log(TEXT("Cleared all streaming targets"));
}

void ACesiumVertexSampler::StopStreamingSpawn()
{
	bStreamingActive = false;
	
	Log(TEXT("========================================"));
	Log(FString::Printf(TEXT("STREAMING SPAWN STOPPED! Total instances: %d across %d targets"), 
		TotalStreamedInstances, StreamingTargets.Num()));
}

// ============== LOGGING ==============

void ACesiumVertexSampler::Log(const FString& Message)
{
	if (bEnableLogging)
	{
		UE_LOG(LogTemp, Log, TEXT("[CesiumVertexSampler] %s"), *Message);
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, FString::Printf(TEXT("[CesiumVtx] %s"), *Message));
	}
}

void ACesiumVertexSampler::LogWarning(const FString& Message)
{
	UE_LOG(LogTemp, Warning, TEXT("[CesiumVertexSampler] %s"), *Message);
	GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, FString::Printf(TEXT("[CesiumVtx WARNING] %s"), *Message));
}

void ACesiumVertexSampler::LogError(const FString& Message)
{
	UE_LOG(LogTemp, Error, TEXT("[CesiumVertexSampler] %s"), *Message);
	GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, FString::Printf(TEXT("[CesiumVtx ERROR] %s"), *Message));
}

// ============== RANDOM HELPERS ==============

FVector ACesiumVertexSampler::GetRandomOffset(float Range)
{
	float X = RandomStream.FRandRange(-Range, Range);
	float Y = RandomStream.FRandRange(-Range, Range);
	float Z = RandomStream.FRandRange(-Range * 0.1f, Range * 0.1f);
	return FVector(X, Y, Z);
}

FVector ACesiumVertexSampler::GetRandomScale(const FVector& MinScale, const FVector& MaxScale)
{
	float ScaleX = RandomStream.FRandRange(MinScale.X, MaxScale.X);
	float ScaleY = RandomStream.FRandRange(MinScale.Y, MaxScale.Y);
	float ScaleZ = RandomStream.FRandRange(MinScale.Z, MaxScale.Z);
	return FVector(ScaleX, ScaleY, ScaleZ);
}

FRotator ACesiumVertexSampler::GetRandomRotation()
{
	float Yaw = RandomStream.FRandRange(0.0f, 360.0f);
	return FRotator(0.0f, Yaw, 0.0f);
}

// ============== MAIN FUNCTIONS ==============

FCesiumSpawnResult ACesiumVertexSampler::SpawnOnCesiumTileset(
	AActor* CesiumTileset,
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	FCesiumSpawnConfig Config)
{
	FCesiumSpawnResult Result;

	Log(TEXT("========================================"));
	Log(TEXT("SpawnOnCesiumTileset - DIRECT VERTEX MODE (No tracing!)"));
	Log(FString::Printf(TEXT("Config - Seed: %d, Range: %.1f, PerVertex: %d, Skip: %d"),
		Config.RandomSeed, Config.SpawnRange, Config.InstancesPerVertex, Config.VertexSkipCount));

	if (!CesiumTileset)
	{
		LogError(TEXT("CesiumTileset is NULL!"));
		return Result;
	}

	if (!TargetHISM)
	{
		LogError(TEXT("TargetHISM is NULL!"));
		return Result;
	}

	if (!TargetHISM->GetStaticMesh())
	{
		LogError(TEXT("TargetHISM has no StaticMesh assigned!"));
		return Result;
	}

	// Initialize random stream
	RandomStream.Initialize(Config.RandomSeed);

	// Get vertices directly from Cesium components
	TArray<FVector> Vertices = GetCesiumVertices(CesiumTileset, Config.VertexSkipCount);

	Result.TotalVerticesFound = Vertices.Num() * Config.VertexSkipCount;
	Result.VerticesUsed = Vertices.Num();

	if (Vertices.Num() == 0)
	{
		LogWarning(TEXT("No vertices found! Tileset may not be loaded yet."));
		return Result;
	}

	Log(FString::Printf(TEXT("Spawning on %d vertex positions..."), Vertices.Num()));

	// Pre-allocate
	int32 TotalInstances = Vertices.Num() * Config.InstancesPerVertex;
	TargetHISM->PreAllocateInstancesMemory(TotalInstances);

	int32 InstanceCount = 0;

	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		const FVector& VertexPos = Vertices[i];

		for (int32 j = 0; j < Config.InstancesPerVertex; j++)
		{
			FVector Offset = GetRandomOffset(Config.SpawnRange);
			FVector SpawnPos = VertexPos + Offset;
			SpawnPos.Z += Config.HeightOffset;

			FRotator Rotation = GetRandomRotation();
			FVector Scale = GetRandomScale(Config.MinScale, Config.MaxScale);

			FTransform InstanceTransform;
			InstanceTransform.SetLocation(SpawnPos);
			InstanceTransform.SetRotation(Rotation.Quaternion());
			InstanceTransform.SetScale3D(Scale);

			TargetHISM->AddInstance(InstanceTransform, true);
			InstanceCount++;
		}

		if (bEnableLogging && i > 0 && i % 10000 == 0)
		{
			Log(FString::Printf(TEXT("Progress: %d / %d vertices (%d instances)"), i, Vertices.Num(), InstanceCount));
		}
	}

	TargetHISM->MarkRenderStateDirty();

	Result.InstancesSpawned = InstanceCount;

	Log(TEXT("========================================"));
	Log(FString::Printf(TEXT("COMPLETED! Spawned %d instances from %d vertices (NO TRACING!)"), InstanceCount, Vertices.Num()));

	return Result;
}

TArray<FVector> ACesiumVertexSampler::GetCesiumVertices(AActor* CesiumTileset, int32 SkipCount)
{
	TArray<FVector> AllVertices;

	if (!CesiumTileset)
	{
		LogError(TEXT("GetCesiumVertices: CesiumTileset is NULL!"));
		return AllVertices;
	}

	// SAFETY: Always clamp SkipCount to prevent infinite loop
	int32 SafeSkip = FMath::Max(1, SkipCount);

	// CesiumGltfPrimitiveComponent inherits from UStaticMeshComponent
	// So we just get all StaticMeshComponents - no private headers needed!
	TArray<UStaticMeshComponent*> MeshComponents;
	CesiumTileset->GetComponents<UStaticMeshComponent>(MeshComponents);

	Log(FString::Printf(TEXT("Found %d StaticMeshComponents (includes Cesium tiles)"), MeshComponents.Num()));

	if (MeshComponents.Num() == 0)
	{
		// Fallback: try getting all primitive components
		LogWarning(TEXT("No StaticMeshComponent found, trying generic primitives..."));
		
		TArray<UPrimitiveComponent*> PrimitiveComponents;
		CesiumTileset->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
		
		Log(FString::Printf(TEXT("Found %d primitive components"), PrimitiveComponents.Num()));
		
		for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
		{
			if (!IsValid(PrimComp)) continue;
			
			TArray<FVector> CompVertices = ExtractVerticesFromComponent(PrimComp);
			for (int32 i = 0; i < CompVertices.Num(); i += SafeSkip)
			{
				AllVertices.Add(CompVertices[i]);
			}
		}
	}
	else
	{
		// Process StaticMeshComponents directly (this includes CesiumGltfPrimitiveComponent!)
		int32 NumProcessed = 0;
		
		for (UStaticMeshComponent* MeshComp : MeshComponents)
		{
			if (!IsValid(MeshComp) || !MeshComp->IsVisible())
			{
				continue;
			}

			TArray<FVector> CompVertices = ExtractVerticesFromComponent(MeshComp);
			
			for (int32 i = 0; i < CompVertices.Num(); i += SafeSkip)
			{
				AllVertices.Add(CompVertices[i]);
			}

			NumProcessed++;

			if (bEnableLogging && NumProcessed <= 3)
			{
				Log(FString::Printf(TEXT("  Component %d: extracted %d vertices"), 
					NumProcessed, CompVertices.Num()));
			}
		}

		if (NumProcessed > 3)
		{
			Log(FString::Printf(TEXT("  ... and %d more components"), NumProcessed - 3));
		}
	}

	Log(FString::Printf(TEXT("Total vertices extracted: %d (skip count: %d)"), AllVertices.Num(), SafeSkip));

	return AllVertices;
}

TArray<FVector> ACesiumVertexSampler::ExtractVerticesFromComponent(UPrimitiveComponent* Component)
{
	TArray<FVector> Vertices;

	// CRITICAL: Use IsValid() for Cesium components - they can be destroyed at any time
	if (!IsValid(Component))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Component is NULL or destroyed"));
		return Vertices;
	}

	// Check component is registered and visible
	if (!Component->IsRegistered() || !Component->IsVisible())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Component not registered or not visible"));
		return Vertices;
	}

	// CesiumGltfPrimitiveComponent inherits from UStaticMeshComponent
	UStaticMeshComponent* MeshComp = Cast<UStaticMeshComponent>(Component);
	
	if (MeshComp && MeshComp->GetStaticMesh())
	{
		UStaticMesh* Mesh = MeshComp->GetStaticMesh();
		
		// Check mesh is fully loaded
		if (Mesh->HasPendingInitOrStreaming())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Mesh has pending init/streaming"));
			return Vertices;
		}
		
		// Cache RenderData once to avoid race conditions from multiple GetRenderData() calls
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: No RenderData or LODResources"));
			return Vertices;
		}
		
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
		uint32 NumVertices = PositionBuffer.GetNumVertices();
		
		if (NumVertices == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: NumVertices is 0"));
			return Vertices;
		}
		
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] ExtractVertices: Found %d vertices, proceeding..."), NumVertices);
		
		// Cap vertices to prevent excessive processing
		const uint32 MaxVerticesPerComponent = 50000;
		if (NumVertices > MaxVerticesPerComponent)
		{
			NumVertices = MaxVerticesPerComponent;
		}
		
		// Fix 1: Comprehensive transform validation for Cesium streaming tiles (UE 5.7 compatible)
		const FTransform CompTransform = MeshComp->GetComponentToWorld();
		
		// Check transform is valid (handles NaN, uninitialized, etc.)
		if (!CompTransform.IsValid())
		{
			return Vertices;
		}
		
		// Check scale is valid (prevents NIL matrix / non-invertible matrix errors)
		const FVector Scale = CompTransform.GetScale3D();
		if (Scale.ContainsNaN() ||
			!FMath::IsFinite(Scale.X) ||
			!FMath::IsFinite(Scale.Y) ||
			!FMath::IsFinite(Scale.Z) ||
			Scale.IsNearlyZero(1e-6f))
		{
			return Vertices;
		}
		
		// Check location and rotation are valid
		const FVector Location = CompTransform.GetLocation();
		if (Location.ContainsNaN() ||
			!FMath::IsFinite(Location.X) ||
			!FMath::IsFinite(Location.Y) ||
			!FMath::IsFinite(Location.Z) ||
			!CompTransform.GetRotation().IsNormalized())
		{
			return Vertices;
		}
		
		// NOTE: Transform stability check removed - Cesium replaces component pointers when
		// tiles refine, so the "same" tile has a different pointer each time. This was causing
		// infinite deferral. SEH protection + coordinate validation handles bad data instead.
		
		// Get component bounds for reference (used for logging only now)
		FBoxSphereBounds Bounds = MeshComp->Bounds;
		
		// ========================================================================
		// UNITY APPROACH: Copy the entire vertex buffer to safe memory FIRST
		// This eliminates the race condition where Cesium can free the buffer
		// mid-iteration. We copy once, then iterate over our safe copy.
		// ========================================================================
		
		// Get raw buffer data pointer and validate
		const void* RawBufferData = PositionBuffer.GetVertexData();
		if (!RawBufferData)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Buffer data pointer is null"));
			return Vertices;
		}
		
		const uint32 Stride = PositionBuffer.GetStride();
		if (Stride == 0 || Stride < sizeof(FVector3f))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Invalid stride %d"), Stride);
			return Vertices;
		}
		
		// Calculate total buffer size
		const SIZE_T TotalBufferSize = static_cast<SIZE_T>(NumVertices) * Stride;
		
		// Sanity check - max 50MB buffer copy (prevents memory bombs)
		const SIZE_T MaxBufferSize = 50 * 1024 * 1024;
		if (TotalBufferSize > MaxBufferSize)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Buffer too large (%llu bytes), capping"), TotalBufferSize);
			NumVertices = static_cast<uint32>(MaxBufferSize / Stride);
		}
		
		const SIZE_T ActualBufferSize = static_cast<SIZE_T>(NumVertices) * Stride;
		
		// Allocate our safe copy
		TArray<uint8> SafeVertexData;
		SafeVertexData.SetNumUninitialized(ActualBufferSize);
		
		// Copy the buffer using SEH-protected memcpy
		// If Cesium frees the buffer during this copy, we catch it and abort
		if (!SafeMemcpyWithSEH(SafeVertexData.GetData(), RawBufferData, ActualBufferSize))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ExtractVertices: Buffer was freed during copy (SEH caught)"));
			return Vertices;
		}
		
		// SUCCESS! We now have a safe copy of the vertex data
		// Cesium can free the original buffer - we don't care anymore!
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] Successfully copied %llu bytes to safe buffer, processing %d vertices..."), 
			ActualBufferSize, NumVertices);
		
		Vertices.Reserve(NumVertices);
		
		int32 NaNLocalCount = 0;
		int32 NaNWorldCount = 0;
		int32 AbsurdCoordCount = 0;
		int32 AddedCount = 0;
		int32 EcefConvertedCount = 0;
		
		// Iterate over our SAFE COPY - no more SEH needed per-vertex!
		const uint8* SafeDataPtr = SafeVertexData.GetData();
		
		for (uint32 i = 0; i < NumVertices; i++)
		{
			// Read from our safe copy using pointer arithmetic
			const FVector3f* LocalPosPtr = reinterpret_cast<const FVector3f*>(SafeDataPtr + (i * Stride));
			const FVector3f LocalPos = *LocalPosPtr;
			
			// Skip invalid vertex positions (NaN/Inf from corrupted data)
			if (!FMath::IsFinite(LocalPos.X) ||
				!FMath::IsFinite(LocalPos.Y) ||
				!FMath::IsFinite(LocalPos.Z))
			{
				NaNLocalCount++;
				continue;
			}
			
			// Transform local vertex to world coordinates using component transform
			FVector WorldPos = CompTransform.TransformPosition(FVector(LocalPos));
			
			// Only add valid world positions
			if (WorldPos.ContainsNaN() ||
				!FMath::IsFinite(WorldPos.X) ||
				!FMath::IsFinite(WorldPos.Y) ||
				!FMath::IsFinite(WorldPos.Z))
			{
				NaNWorldCount++;
				continue;
			}
			
			// ========================================================================
			// PACKAGED BUILD FIX: Detect ECEF coordinates and convert to Unreal
			// In packaged builds, ComponentToWorld may give ECEF coordinates (millions of units)
			// instead of proper Unreal world coordinates.
			// 
			// Detection: If coordinate magnitude is > 1,000,000 (10km), it's likely ECEF
			// ECEF coords are measured from Earth's center (~6,371 km radius)
			// ========================================================================
			static constexpr double EcefThreshold = 1000000.0; // 10km in cm
			const double CoordMagnitude = WorldPos.Size();
			
			if (CoordMagnitude > EcefThreshold && CachedGeoreference)
			{
				// Convert ECEF to Unreal using Georeference
				WorldPos = CachedGeoreference->TransformEarthCenteredEarthFixedPositionToUnreal(WorldPos);
				EcefConvertedCount++;
				
				// Validate converted position
				if (WorldPos.ContainsNaN() ||
					!FMath::IsFinite(WorldPos.X) ||
					!FMath::IsFinite(WorldPos.Y) ||
					!FMath::IsFinite(WorldPos.Z))
				{
					NaNWorldCount++;
					continue;
				}
			}
			
			// Filter truly absurd coordinates (prevents memory issues)
			// After potential ECEF conversion, coords should be reasonable
			// Using 1e9 (10,000 km) - generous but catches garbage
			static constexpr double MaxReasonableWorldCoord = 1e9;
			if (FMath::Abs(WorldPos.X) > MaxReasonableWorldCoord ||
				FMath::Abs(WorldPos.Y) > MaxReasonableWorldCoord ||
				FMath::Abs(WorldPos.Z) > MaxReasonableWorldCoord)
			{
				AbsurdCoordCount++;
				continue;
			}
			
			Vertices.Add(WorldPos);
			AddedCount++;
		}
		
		UE_LOG(LogTemp, Log, TEXT("[CesiumVtx] Vertex extraction: Added=%d, ECEF_Converted=%d, NaNLocal=%d, NaNWorld=%d, Absurd=%d, HasGeoreference=%s"),
			AddedCount, EcefConvertedCount, NaNLocalCount, NaNWorldCount, AbsurdCoordCount,
			CachedGeoreference ? TEXT("YES") : TEXT("NO"));
		
		// Log transform info for debugging Cesium coordinate issues
		if (AddedCount == 0 && NumVertices > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CesiumVtx] ALL VERTICES FILTERED! Transform: Loc=(%.1f, %.1f, %.1f) Scale=(%.4f, %.4f, %.4f)"),
				Location.X, Location.Y, Location.Z, Scale.X, Scale.Y, Scale.Z);
		}
		
		// Return whatever vertices we extracted (or empty if none)
	}

	return Vertices;
}

int32 ACesiumVertexSampler::GetCesiumVertexCount(AActor* CesiumTileset)
{
	if (!CesiumTileset)
	{
		return 0;
	}

	int32 TotalCount = 0;

	// Use UStaticMeshComponent - Cesium's components inherit from this!
	TArray<UStaticMeshComponent*> MeshComponents;
	CesiumTileset->GetComponents<UStaticMeshComponent>(MeshComponents);

	for (UStaticMeshComponent* MeshComp : MeshComponents)
	{
		// CRITICAL: Use IsValid() - Cesium can destroy components leaving dangling pointers
		if (!IsValid(MeshComp) || !MeshComp->IsVisible())
		{
			continue;
		}

		if (MeshComp->GetStaticMesh())
		{
			UStaticMesh* Mesh = MeshComp->GetStaticMesh();
			if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
			{
				TotalCount += Mesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices();
			}
		}
	}

	Log(FString::Printf(TEXT("Total vertex count: %d from %d components"), TotalCount, MeshComponents.Num()));

	return TotalCount;
}

void ACesiumVertexSampler::ClearAllInstances(UHierarchicalInstancedStaticMeshComponent* TargetHISM)
{
	if (!TargetHISM)
	{
		LogWarning(TEXT("ClearAllInstances: TargetHISM is NULL!"));
		return;
	}

	int32 PreviousCount = TargetHISM->GetInstanceCount();
	TargetHISM->ClearInstances();
	
	Log(FString::Printf(TEXT("Cleared %d instances"), PreviousCount));
}
