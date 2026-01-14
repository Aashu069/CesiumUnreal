// CesiumVertexSampler.cpp
// Direct vertex extraction from Cesium 3D Tilesets - NO line tracing!
// Uses standard UE APIs only - CesiumGltfPrimitiveComponent IS a UStaticMeshComponent!

#include "CesiumVertexSampler.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"

ACesiumVertexSampler::ACesiumVertexSampler()
{
	PrimaryActorTick.bCanEverTick = true; // Enable tick for streaming
}

void ACesiumVertexSampler::BeginPlay()
{
	Super::BeginPlay();
	Log(TEXT("CesiumVertexSampler initialized - OPTIMIZED vertex extraction (batch spawning, frame limiting)"));
}

void ACesiumVertexSampler::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bStreamingActive || StreamingTargets.Num() == 0)
	{
		return;
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
			if (!MeshComp || !MeshComp->IsVisible())
			{
				continue;
			}

			// Skip if we already processed or queued this component
			if (ProcessedComponents.Contains(MeshComp))
			{
				continue;
			}

			// Mark as processed and add to pending
			ProcessedComponents.Add(MeshComp);
			PendingComponents.Add(MeshComp);
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

			if (!MeshComp || !MeshComp->IsVisible())
			{
				continue;
			}

			NewComponentsProcessed++;

			// Extract vertices once per component
			TArray<FVector> CompVertices = ExtractVerticesFromComponent(MeshComp);

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
	if (!Target.HISM || Vertices.Num() == 0) return;

	// Pre-calculate how many transforms we'll create
	int32 VerticesUsed = (Vertices.Num() + Target.Config.VertexSkipCount - 1) / Target.Config.VertexSkipCount;
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

	for (int32 i = 0; i < Vertices.Num() && AddedCount < TotalTransforms; i += Target.Config.VertexSkipCount)
	{
		const FVector& VertexPos = Vertices[i];

		for (int32 j = 0; j < Target.Config.InstancesPerVertex && AddedCount < TotalTransforms; j++)
		{
			// Use target's own random stream for consistent results per foliage type
			float RangeX = Target.RandomStream.FRandRange(-Target.Config.SpawnRange, Target.Config.SpawnRange);
			float RangeY = Target.RandomStream.FRandRange(-Target.Config.SpawnRange, Target.Config.SpawnRange);
			float RangeZ = Target.RandomStream.FRandRange(-Target.Config.SpawnRange * 0.1f, Target.Config.SpawnRange * 0.1f);
			
			FVector SpawnPos = VertexPos + FVector(RangeX, RangeY, RangeZ);
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

	// BATCH ADD - Much faster than adding one by one!
	if (BatchTransforms.Num() > 0)
	{
		Target.HISM->AddInstances(BatchTransforms, false);  // false = don't mark dirty yet
		Target.HISM->MarkRenderStateDirty();  // Mark dirty once after all adds
		
		Target.InstancesSpawned += BatchTransforms.Num();
		TotalStreamedInstances += BatchTransforms.Num();
		InstancesThisFrame += BatchTransforms.Num();
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

	SkipCount = FMath::Max(1, SkipCount);

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
			TArray<FVector> CompVertices = ExtractVerticesFromComponent(PrimComp);
			for (int32 i = 0; i < CompVertices.Num(); i += SkipCount)
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
			if (!MeshComp || !MeshComp->IsVisible())
			{
				continue;
			}

			TArray<FVector> CompVertices = ExtractVerticesFromComponent(MeshComp);
			
			for (int32 i = 0; i < CompVertices.Num(); i += SkipCount)
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

	Log(FString::Printf(TEXT("Total vertices extracted: %d (skip count: %d)"), AllVertices.Num(), SkipCount));

	return AllVertices;
}

TArray<FVector> ACesiumVertexSampler::ExtractVerticesFromComponent(UPrimitiveComponent* Component)
{
	TArray<FVector> Vertices;

	if (!Component)
	{
		return Vertices;
	}

	// CesiumGltfPrimitiveComponent inherits from UStaticMeshComponent
	// So we cast to UStaticMeshComponent - works for Cesium tiles!
	UStaticMeshComponent* MeshComp = Cast<UStaticMeshComponent>(Component);
	
	if (MeshComp && MeshComp->GetStaticMesh())
	{
		UStaticMesh* Mesh = MeshComp->GetStaticMesh();
		
		// Try to get render data (contains actual vertex positions)
		if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LODResources = Mesh->GetRenderData()->LODResources[0];
			const FPositionVertexBuffer& PositionBuffer = LODResources.VertexBuffers.PositionVertexBuffer;
			uint32 NumVertices = PositionBuffer.GetNumVertices();
			
			if (NumVertices > 0)
			{
				FTransform CompTransform = MeshComp->GetComponentTransform();
				Vertices.Reserve(NumVertices);
				
				for (uint32 i = 0; i < NumVertices; i++)
				{
					FVector3f LocalPos = PositionBuffer.VertexPosition(i);
					FVector WorldPos = CompTransform.TransformPosition(FVector(LocalPos.X, LocalPos.Y, LocalPos.Z));
					Vertices.Add(WorldPos);
				}
				
				return Vertices;
			}
		}
		
		// Fallback: Try physics body (collision mesh)
		if (MeshComp->GetBodySetup())
		{
			UBodySetup* BodySetup = MeshComp->GetBodySetup();
			FTransform CompTransform = MeshComp->GetComponentTransform();
			
			// Get triangles from convex elements
			for (const FKConvexElem& ConvexElem : BodySetup->AggGeom.ConvexElems)
			{
				for (const FVector& Vertex : ConvexElem.VertexData)
				{
					FVector WorldPos = CompTransform.TransformPosition(Vertex);
					Vertices.Add(WorldPos);
				}
			}
			
			if (Vertices.Num() > 0)
			{
				return Vertices;
			}
		}
	}
	
	// Final fallback: Use bounds sampling
	if (Vertices.Num() == 0)
	{
		FBoxSphereBounds Bounds = Component->CalcBounds(Component->GetComponentTransform());
		
		if (Bounds.BoxExtent.Size() >= 10.0f)
		{
			FVector Origin = Bounds.Origin;
			FVector Extent = Bounds.BoxExtent;
			
			// Sample multiple points within the bounds (grid pattern)
			int32 GridSize = FMath::Clamp(FMath::CeilToInt(Extent.Size() / 200.0f), 2, 5);
			
			for (int32 x = 0; x <= GridSize; x++)
			{
				for (int32 y = 0; y <= GridSize; y++)
				{
					float fx = (float)x / (float)GridSize - 0.5f;
					float fy = (float)y / (float)GridSize - 0.5f;
					FVector Point = Origin + FVector(Extent.X * fx * 2.0f, Extent.Y * fy * 2.0f, 0);
					Vertices.Add(Point);
				}
			}
		}
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
		if (!MeshComp || !MeshComp->IsVisible())
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
