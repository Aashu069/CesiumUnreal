// MeshVertexSampler.cpp
// Implementation of vertex sampling and foliage spawning

#include "MeshVertexSampler.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"

AMeshVertexSampler::AMeshVertexSampler()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AMeshVertexSampler::BeginPlay()
{
	Super::BeginPlay();
	LogMessage(TEXT("MeshVertexSampler initialized and ready."), true);
}

void AMeshVertexSampler::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

// ============== LOGGING HELPERS ==============

void AMeshVertexSampler::LogMessage(const FString& Message, bool bForceLog)
{
	if (bEnableVerboseLogging || bForceLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[MeshVertexSampler] %s"), *Message);
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::Printf(TEXT("[Sampler] %s"), *Message));
	}
}

void AMeshVertexSampler::LogWarning(const FString& Message)
{
	UE_LOG(LogTemp, Warning, TEXT("[MeshVertexSampler] %s"), *Message);
	GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, FString::Printf(TEXT("[Sampler WARNING] %s"), *Message));
}

void AMeshVertexSampler::LogError(const FString& Message)
{
	UE_LOG(LogTemp, Error, TEXT("[MeshVertexSampler] %s"), *Message);
	GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, FString::Printf(TEXT("[Sampler ERROR] %s"), *Message));
}

// ============== RANDOM HELPERS ==============

FVector AMeshVertexSampler::GetRandomOffset(float Range)
{
	float X = RandomStream.FRandRange(-Range, Range);
	float Y = RandomStream.FRandRange(-Range, Range);
	float Z = RandomStream.FRandRange(-Range * 0.1f, Range * 0.1f); // Smaller Z offset
	return FVector(X, Y, Z);
}

FVector AMeshVertexSampler::GetRandomScale(const FVector& MinScale, const FVector& MaxScale)
{
	float ScaleX = RandomStream.FRandRange(MinScale.X, MaxScale.X);
	float ScaleY = RandomStream.FRandRange(MinScale.Y, MaxScale.Y);
	float ScaleZ = RandomStream.FRandRange(MinScale.Z, MaxScale.Z);
	return FVector(ScaleX, ScaleY, ScaleZ);
}

FRotator AMeshVertexSampler::GetRandomRotation()
{
	float Yaw = RandomStream.FRandRange(0.0f, 360.0f);
	return FRotator(0.0f, Yaw, 0.0f);
}

// ============== MAIN FUNCTIONS ==============

FVertexSampleResult AMeshVertexSampler::SampleAndSpawnFoliage(
	AActor* SourceTileset,
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	FSpawnConfiguration Config)
{
	FVertexSampleResult Result;

	LogMessage(TEXT("========================================"), true);
	LogMessage(TEXT("Starting SampleAndSpawnFoliage..."), true);
	LogMessage(FString::Printf(TEXT("Config - Seed: %d, Range: %.1f, InstancesPerVertex: %d, SkipCount: %d"), 
		Config.RandomSeed, Config.SpawnRange, Config.InstancesPerVertex, Config.VertexSkipCount), true);

	// Validate inputs
	if (!SourceTileset)
	{
		LogError(TEXT("SourceTileset is NULL! Cannot proceed."));
		return Result;
	}

	if (!TargetHISM)
	{
		LogError(TEXT("TargetHISM is NULL! Cannot proceed."));
		return Result;
	}

	if (!TargetHISM->GetStaticMesh())
	{
		LogError(TEXT("TargetHISM has no StaticMesh assigned! Please assign a mesh."));
		return Result;
	}

	// Initialize random stream with seed
	RandomStream.Initialize(Config.RandomSeed);
	LogMessage(FString::Printf(TEXT("Random stream initialized with seed: %d"), Config.RandomSeed));

	// Get all vertices from the tileset
	LogMessage(TEXT("Extracting vertices from tileset..."));
	TArray<FVector> AllVertices = GetVerticesFromActor(SourceTileset, Config.VertexSkipCount);
	
	Result.TotalVerticesFound = AllVertices.Num() * Config.VertexSkipCount; // Approximate total
	Result.VerticesUsed = AllVertices.Num();

	LogMessage(FString::Printf(TEXT("Vertices extracted: %d (using every %d vertex)"), 
		AllVertices.Num(), Config.VertexSkipCount), true);

	if (AllVertices.Num() == 0)
	{
		LogWarning(TEXT("No vertices found! Check if tileset has mesh components loaded."));
		return Result;
	}

	// Spawn instances at vertex positions
	LogMessage(TEXT("Spawning foliage instances..."));
	Result.InstancesSpawned = SpawnInstancesAtPositions(TargetHISM, AllVertices, Config);

	LogMessage(TEXT("========================================"), true);
	LogMessage(FString::Printf(TEXT("COMPLETED! Spawned %d foliage instances"), Result.InstancesSpawned), true);
	LogMessage(TEXT("========================================"), true);

	return Result;
}

TArray<FVector> AMeshVertexSampler::GetVerticesFromActor(AActor* SourceActor, int32 SkipCount)
{
	TArray<FVector> AllVertices;

	if (!SourceActor)
	{
		LogError(TEXT("GetVerticesFromActor: SourceActor is NULL!"));
		return AllVertices;
	}

	SkipCount = FMath::Max(1, SkipCount); // Ensure at least 1

	LogMessage(FString::Printf(TEXT("Getting vertices from actor: %s"), *SourceActor->GetName()));

	// Get all static mesh components
	TArray<UStaticMeshComponent*> MeshComponents;
	SourceActor->GetComponents<UStaticMeshComponent>(MeshComponents);

	LogMessage(FString::Printf(TEXT("Found %d StaticMeshComponents"), MeshComponents.Num()));

	int32 ComponentIndex = 0;
	for (UStaticMeshComponent* MeshComp : MeshComponents)
	{
		if (!MeshComp)
		{
			continue;
		}

		TArray<FVector> ComponentVertices = GetVerticesFromComponent(MeshComp);
		
		// Apply skip count to reduce vertex density
		for (int32 i = 0; i < ComponentVertices.Num(); i += SkipCount)
		{
			AllVertices.Add(ComponentVertices[i]);
		}

		if (bEnableVerboseLogging && ComponentIndex < 5) // Log first 5 components
		{
			LogMessage(FString::Printf(TEXT("  Component %d: %d vertices (using %d)"), 
				ComponentIndex, ComponentVertices.Num(), ComponentVertices.Num() / SkipCount));
		}

		ComponentIndex++;
	}

	if (ComponentIndex > 5)
	{
		LogMessage(FString::Printf(TEXT("  ... and %d more components"), ComponentIndex - 5));
	}

	LogMessage(FString::Printf(TEXT("Total vertices collected: %d"), AllVertices.Num()));

	return AllVertices;
}

TArray<FVector> AMeshVertexSampler::GetVerticesFromComponent(UStaticMeshComponent* MeshComponent)
{
	TArray<FVector> Vertices;

	if (!MeshComponent)
	{
		return Vertices;
	}

	UStaticMesh* StaticMesh = MeshComponent->GetStaticMesh();
	if (!StaticMesh)
	{
		return Vertices;
	}

	// Get the component's world transform
	FTransform ComponentTransform = MeshComponent->GetComponentTransform();

	// Use GetPhysicsTriMeshData or GetStaticMeshDescription for vertex access
	// For simplicity, we'll use the mesh's bounding box corners and sample points
	FBoxSphereBounds Bounds = MeshComponent->Bounds;
	
	// Sample the bounding box to create spawn points
	// This is a simpler approach that works with all mesh types
	FVector Origin = Bounds.Origin;
	FVector Extent = Bounds.BoxExtent;
	
	// Add center point
	Vertices.Add(Origin);
	
	// Add corner points
	Vertices.Add(Origin + FVector(Extent.X, Extent.Y, 0));
	Vertices.Add(Origin + FVector(-Extent.X, Extent.Y, 0));
	Vertices.Add(Origin + FVector(Extent.X, -Extent.Y, 0));
	Vertices.Add(Origin + FVector(-Extent.X, -Extent.Y, 0));
	
	// Add edge midpoints for more coverage
	Vertices.Add(Origin + FVector(Extent.X, 0, 0));
	Vertices.Add(Origin + FVector(-Extent.X, 0, 0));
	Vertices.Add(Origin + FVector(0, Extent.Y, 0));
	Vertices.Add(Origin + FVector(0, -Extent.Y, 0));

	return Vertices;
}

int32 AMeshVertexSampler::SpawnInstancesAtPositions(
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	const TArray<FVector>& Positions,
	FSpawnConfiguration Config)
{
	if (!TargetHISM)
	{
		LogError(TEXT("SpawnInstancesAtPositions: TargetHISM is NULL!"));
		return 0;
	}

	int32 InstanceCount = 0;
	int32 TotalToSpawn = Positions.Num() * Config.InstancesPerVertex;
	int32 TraceHits = 0;
	int32 TraceMisses = 0;

	LogMessage(FString::Printf(TEXT("Spawning instances: %d positions x %d per vertex = %d total"), 
		Positions.Num(), Config.InstancesPerVertex, TotalToSpawn));

	// Pre-allocate for better performance
	TargetHISM->PreAllocateInstancesMemory(TotalToSpawn);

	// Get world for line traces
	UWorld* World = TargetHISM->GetWorld();
	if (!World)
	{
		LogError(TEXT("Could not get World for line traces!"));
		return 0;
	}

	for (int32 i = 0; i < Positions.Num(); i++)
	{
		const FVector& BasePosition = Positions[i];

		// Spawn multiple instances per vertex
		for (int32 j = 0; j < Config.InstancesPerVertex; j++)
		{
			// Calculate spawn position with random offset
			FVector Offset = GetRandomOffset(Config.SpawnRange);
			FVector SpawnPosition = BasePosition + Offset;

			// LINE TRACE DOWN to find the ground surface
			FVector TraceStart = SpawnPosition + FVector(0, 0, 50000.0f); // Start high above
			FVector TraceEnd = SpawnPosition - FVector(0, 0, 100000.0f);   // Trace far down

			FHitResult HitResult;
			FCollisionQueryParams QueryParams;
			QueryParams.bTraceComplex = true;
			QueryParams.bReturnFaceIndex = false;

			bool bHit = World->LineTraceSingleByChannel(
				HitResult,
				TraceStart,
				TraceEnd,
				ECC_Visibility,
				QueryParams
			);

			if (bHit)
			{
				// Use the hit location as spawn point
				SpawnPosition = HitResult.Location;
				SpawnPosition.Z += Config.HeightOffset;
				TraceHits++;
			}
			else
			{
				// No ground found, skip this instance
				TraceMisses++;
				continue;
			}

			// Calculate random rotation and scale
			FRotator Rotation = GetRandomRotation();
			FVector Scale = GetRandomScale(Config.MinScale, Config.MaxScale);

			// Create transform
			FTransform InstanceTransform;
			InstanceTransform.SetLocation(SpawnPosition);
			InstanceTransform.SetRotation(Rotation.Quaternion());
			InstanceTransform.SetScale3D(Scale);

			// Add instance
			TargetHISM->AddInstance(InstanceTransform, true);
			InstanceCount++;
		}

		// Log progress every 1000 vertices
		if (bEnableVerboseLogging && i > 0 && i % 1000 == 0)
		{
			LogMessage(FString::Printf(TEXT("Progress: %d / %d vertices processed (%d instances, %d hits, %d misses)"), 
				i, Positions.Num(), InstanceCount, TraceHits, TraceMisses));
		}
	}

	// Force update of instance data
	TargetHISM->MarkRenderStateDirty();

	LogMessage(FString::Printf(TEXT("Line traces: %d hits, %d misses"), TraceHits, TraceMisses), true);

	return InstanceCount;
}

int32 AMeshVertexSampler::SpawnInstancesOnActor(
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	const TArray<FVector>& Positions,
	FSpawnConfiguration Config,
	AActor* TargetSurfaceActor)
{
	if (!TargetHISM)
	{
		LogError(TEXT("SpawnInstancesOnActor: TargetHISM is NULL!"));
		return 0;
	}

	if (!TargetSurfaceActor)
	{
		LogError(TEXT("SpawnInstancesOnActor: TargetSurfaceActor is NULL!"));
		return 0;
	}

	int32 InstanceCount = 0;
	int32 TotalToSpawn = Positions.Num() * Config.InstancesPerVertex;
	int32 TraceHits = 0;
	int32 TraceMisses = 0;
	int32 WrongActorHits = 0;

	LogMessage(FString::Printf(TEXT("Spawning instances ONLY on '%s': %d positions x %d per vertex = %d total"), 
		*TargetSurfaceActor->GetName(), Positions.Num(), Config.InstancesPerVertex, TotalToSpawn));

	// Pre-allocate for better performance
	TargetHISM->PreAllocateInstancesMemory(TotalToSpawn);

	// Get world for line traces
	UWorld* World = TargetHISM->GetWorld();
	if (!World)
	{
		LogError(TEXT("Could not get World for line traces!"));
		return 0;
	}

	for (int32 i = 0; i < Positions.Num(); i++)
	{
		const FVector& BasePosition = Positions[i];

		// Spawn multiple instances per vertex
		for (int32 j = 0; j < Config.InstancesPerVertex; j++)
		{
			// Calculate spawn position with random offset
			FVector Offset = GetRandomOffset(Config.SpawnRange);
			FVector SpawnPosition = BasePosition + Offset;

			// LINE TRACE DOWN to find the ground surface
			FVector TraceStart = SpawnPosition + FVector(0, 0, 50000.0f);
			FVector TraceEnd = SpawnPosition - FVector(0, 0, 100000.0f);

			FHitResult HitResult;
			FCollisionQueryParams QueryParams;
			QueryParams.bTraceComplex = true;
			QueryParams.bReturnFaceIndex = false;

			bool bHit = World->LineTraceSingleByChannel(
				HitResult,
				TraceStart,
				TraceEnd,
				ECC_Visibility,
				QueryParams
			);

			if (bHit)
			{
				// CHECK if we hit the correct actor (the vegetation tileset, not terrain)
				AActor* HitActor = HitResult.GetActor();
				if (HitActor != TargetSurfaceActor)
				{
					// Hit something else (terrain, etc.) - skip this instance
					WrongActorHits++;
					continue;
				}

				// Use the hit location as spawn point
				SpawnPosition = HitResult.Location;
				SpawnPosition.Z += Config.HeightOffset;
				TraceHits++;
			}
			else
			{
				// No ground found, skip this instance
				TraceMisses++;
				continue;
			}

			// Calculate random rotation and scale
			FRotator Rotation = GetRandomRotation();
			FVector Scale = GetRandomScale(Config.MinScale, Config.MaxScale);

			// Create transform
			FTransform InstanceTransform;
			InstanceTransform.SetLocation(SpawnPosition);
			InstanceTransform.SetRotation(Rotation.Quaternion());
			InstanceTransform.SetScale3D(Scale);

			// Add instance
			TargetHISM->AddInstance(InstanceTransform, true);
			InstanceCount++;
		}

		// Log progress every 1000 vertices
		if (bEnableVerboseLogging && i > 0 && i % 1000 == 0)
		{
			LogMessage(FString::Printf(TEXT("Progress: %d / %d (%d spawned, %d on target, %d wrong actor, %d miss)"), 
				i, Positions.Num(), InstanceCount, TraceHits, WrongActorHits, TraceMisses));
		}
	}

	// Force update of instance data
	TargetHISM->MarkRenderStateDirty();

	LogMessage(FString::Printf(TEXT("Results: %d hits on '%s', %d hits on other actors (skipped), %d misses"), 
		TraceHits, *TargetSurfaceActor->GetName(), WrongActorHits, TraceMisses), true);

	return InstanceCount;
}

// ============== CESIUM/PRIMITIVE COMPONENT HELPERS ==============

TArray<FVector> AMeshVertexSampler::GetSpawnPointsFromPrimitiveComponents(AActor* SourceActor, int32 PointsPerComponent)
{
	TArray<FVector> SpawnPoints;

	if (!SourceActor)
	{
		LogError(TEXT("GetSpawnPointsFromPrimitiveComponents: SourceActor is NULL!"));
		return SpawnPoints;
	}

	// Get ALL primitive components to find the overall bounds
	TArray<UPrimitiveComponent*> PrimitiveComponents;
	SourceActor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);

	LogMessage(FString::Printf(TEXT("Found %d primitive components on '%s'"), 
		PrimitiveComponents.Num(), *SourceActor->GetName()), true);

	if (PrimitiveComponents.Num() == 0)
	{
		return SpawnPoints;
	}

	// Calculate the overall bounding box of all visible components
	FBox TotalBounds(ForceInit);
	int32 ValidComponents = 0;
	
	for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
	{
		if (!PrimComp || !PrimComp->IsVisible())
		{
			continue;
		}

		FBoxSphereBounds CompBounds = PrimComp->Bounds;
		if (CompBounds.BoxExtent.Size() >= 10.0f)
		{
			FBox CompBox(CompBounds.Origin - CompBounds.BoxExtent, CompBounds.Origin + CompBounds.BoxExtent);
			if (ValidComponents == 0)
			{
				TotalBounds = CompBox;
			}
			else
			{
				TotalBounds += CompBox;
			}
			ValidComponents++;
		}
	}

	if (ValidComponents == 0)
	{
		LogWarning(TEXT("No valid components found!"));
		return SpawnPoints;
	}

	FVector BoundsMin = TotalBounds.Min;
	FVector BoundsMax = TotalBounds.Max;
	FVector BoundsSize = BoundsMax - BoundsMin;

	LogMessage(FString::Printf(TEXT("Total bounds: Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f) Size(%.1f, %.1f, %.1f)"),
		BoundsMin.X, BoundsMin.Y, BoundsMin.Z,
		BoundsMax.X, BoundsMax.Y, BoundsMax.Z,
		BoundsSize.X, BoundsSize.Y, BoundsSize.Z), true);

	// Create a DENSE GRID of sample points across the XY bounds
	// PointsPerComponent controls density: higher = denser grid
	// Default spacing of ~100-500 units gives good grass coverage
	float GridSpacing = FMath::Max(50.0f, FMath::Min(BoundsSize.X, BoundsSize.Y) / (PointsPerComponent * 50.0f));
	
	// Clamp to reasonable range for performance - lower minimum = denser
	GridSpacing = FMath::Clamp(GridSpacing, 80.0f, 1300.0f);
	
	int32 GridX = FMath::Max(1, FMath::FloorToInt(BoundsSize.X / GridSpacing));
	int32 GridY = FMath::Max(1, FMath::FloorToInt(BoundsSize.Y / GridSpacing));

	LogMessage(FString::Printf(TEXT("Creating grid: %d x %d = %d points (spacing: %.1f)"),
		GridX, GridY, GridX * GridY, GridSpacing), true);

	// Use center Z of bounds as starting point
	float CenterZ = (BoundsMin.Z + BoundsMax.Z) / 2.0f;

	for (int32 x = 0; x < GridX; x++)
	{
		for (int32 y = 0; y < GridY; y++)
		{
			float PosX = BoundsMin.X + (x + 0.5f) * GridSpacing;
			float PosY = BoundsMin.Y + (y + 0.5f) * GridSpacing;
			
			SpawnPoints.Add(FVector(PosX, PosY, CenterZ));
		}
	}

	LogMessage(FString::Printf(TEXT("Generated %d grid spawn points from %d valid components"), 
		SpawnPoints.Num(), ValidComponents), true);

	return SpawnPoints;
}

int32 AMeshVertexSampler::GetPrimitiveComponentCount(AActor* SourceActor)
{
	if (!SourceActor)
	{
		return 0;
	}

	TArray<UPrimitiveComponent*> PrimitiveComponents;
	SourceActor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);

	int32 VisibleCount = 0;
	for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
	{
		if (PrimComp && PrimComp->IsVisible() && PrimComp->Bounds.BoxExtent.Size() >= 10.0f)
		{
			VisibleCount++;
		}
	}

	LogMessage(FString::Printf(TEXT("Actor '%s' has %d total primitive components (%d visible with geometry)"), 
		*SourceActor->GetName(), PrimitiveComponents.Num(), VisibleCount), true);

	return VisibleCount;
}

FVertexSampleResult AMeshVertexSampler::SampleAndSpawnFromPrimitives(
	AActor* SourceActor,
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	FSpawnConfiguration Config)
{
	FVertexSampleResult Result;

	LogMessage(TEXT("========================================"), true);
	LogMessage(TEXT("Starting SampleAndSpawnFromPrimitives (CESIUM MODE)..."), true);
	LogMessage(FString::Printf(TEXT("Config - Seed: %d, Range: %.1f, InstancesPerVertex: %d"), 
		Config.RandomSeed, Config.SpawnRange, Config.InstancesPerVertex), true);

	if (!SourceActor)
	{
		LogError(TEXT("SourceActor is NULL!"));
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

	// Get spawn points from primitive components
	TArray<FVector> SpawnPoints = GetSpawnPointsFromPrimitiveComponents(SourceActor, 9);

	Result.TotalVerticesFound = SpawnPoints.Num();
	Result.VerticesUsed = SpawnPoints.Num();
	Result.ComponentsProcessed = GetPrimitiveComponentCount(SourceActor);

	if (SpawnPoints.Num() == 0)
	{
		LogWarning(TEXT("No spawn points found! The tileset may not be loaded yet. Try adding a longer delay."));
		return Result;
	}

	// Spawn instances - pass the source actor to filter traces
	Result.InstancesSpawned = SpawnInstancesOnActor(TargetHISM, SpawnPoints, Config, SourceActor);

	LogMessage(TEXT("========================================"), true);
	LogMessage(FString::Printf(TEXT("COMPLETED! Spawned %d instances from %d primitive components"), 
		Result.InstancesSpawned, Result.ComponentsProcessed), true);

	return Result;
}

TArray<FString> AMeshVertexSampler::ListAllComponentTypes(AActor* SourceActor)
{
	TArray<FString> ComponentTypes;

	if (!SourceActor)
	{
		LogError(TEXT("ListAllComponentTypes: SourceActor is NULL!"));
		return ComponentTypes;
	}

	TArray<UActorComponent*> AllComponents;
	SourceActor->GetComponents(AllComponents);

	TMap<FString, int32> TypeCounts;

	for (UActorComponent* Comp : AllComponents)
	{
		if (Comp)
		{
			FString TypeName = Comp->GetClass()->GetName();
			if (TypeCounts.Contains(TypeName))
			{
				TypeCounts[TypeName]++;
			}
			else
			{
				TypeCounts.Add(TypeName, 1);
			}
		}
	}

	LogMessage(FString::Printf(TEXT("Component types on '%s':"), *SourceActor->GetName()), true);
	for (const auto& Pair : TypeCounts)
	{
		FString Entry = FString::Printf(TEXT("  %s: %d"), *Pair.Key, Pair.Value);
		ComponentTypes.Add(Entry);
		LogMessage(Entry, true);
	}

	return ComponentTypes;
}

// ============== UTILITY FUNCTIONS ==============

int32 AMeshVertexSampler::GetMeshComponentCount(AActor* SourceActor)
{
	if (!SourceActor)
	{
		return 0;
	}

	TArray<UStaticMeshComponent*> MeshComponents;
	SourceActor->GetComponents<UStaticMeshComponent>(MeshComponents);
	
	LogMessage(FString::Printf(TEXT("Actor '%s' has %d mesh components"), 
		*SourceActor->GetName(), MeshComponents.Num()), true);

	return MeshComponents.Num();
}

int32 AMeshVertexSampler::GetTotalVertexCount(AActor* SourceActor)
{
	if (!SourceActor)
	{
		return 0;
	}

	int32 TotalVertices = 0;

	TArray<UStaticMeshComponent*> MeshComponents;
	SourceActor->GetComponents<UStaticMeshComponent>(MeshComponents);

	for (UStaticMeshComponent* MeshComp : MeshComponents)
	{
		if (!MeshComp || !MeshComp->GetStaticMesh())
		{
			continue;
		}

		UStaticMesh* Mesh = MeshComp->GetStaticMesh();
		if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
		{
			TotalVertices += Mesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices();
		}
	}

	LogMessage(FString::Printf(TEXT("Actor '%s' has approximately %d total vertices"), 
		*SourceActor->GetName(), TotalVertices), true);

	return TotalVertices;
}

void AMeshVertexSampler::ClearAllInstances(UHierarchicalInstancedStaticMeshComponent* TargetHISM)
{
	if (!TargetHISM)
	{
		LogWarning(TEXT("ClearAllInstances: TargetHISM is NULL!"));
		return;
	}

	int32 PreviousCount = TargetHISM->GetInstanceCount();
	TargetHISM->ClearInstances();
	
	LogMessage(FString::Printf(TEXT("Cleared %d instances from HISM"), PreviousCount), true);
}

// ============== CHILD ACTOR HELPERS ==============

TArray<AActor*> AMeshVertexSampler::GetChildActorsRecursive(AActor* ParentActor, bool bRecursive)
{
	TArray<AActor*> ChildActors;

	if (!ParentActor)
	{
		LogError(TEXT("GetChildActorsRecursive: ParentActor is NULL!"));
		return ChildActors;
	}

	LogMessage(FString::Printf(TEXT("Getting child actors from: %s"), *ParentActor->GetName()));

	// Get directly attached children
	TArray<AActor*> DirectChildren;
	ParentActor->GetAttachedActors(DirectChildren);

	LogMessage(FString::Printf(TEXT("Found %d direct children"), DirectChildren.Num()));

	for (AActor* Child : DirectChildren)
	{
		if (Child)
		{
			ChildActors.Add(Child);

			// Recursively get children of children
			if (bRecursive)
			{
				TArray<AActor*> GrandChildren = GetChildActorsRecursive(Child, true);
				ChildActors.Append(GrandChildren);
			}
		}
	}

	// Also check for ChildActorComponents
	TArray<UChildActorComponent*> ChildActorComponents;
	ParentActor->GetComponents<UChildActorComponent>(ChildActorComponents);

	for (UChildActorComponent* ChildComp : ChildActorComponents)
	{
		if (ChildComp && ChildComp->GetChildActor())
		{
			AActor* ChildActor = ChildComp->GetChildActor();
			if (!ChildActors.Contains(ChildActor))
			{
				ChildActors.Add(ChildActor);

				if (bRecursive)
				{
					TArray<AActor*> GrandChildren = GetChildActorsRecursive(ChildActor, true);
					ChildActors.Append(GrandChildren);
				}
			}
		}
	}

	LogMessage(FString::Printf(TEXT("Total child actors found: %d (recursive: %s)"), 
		ChildActors.Num(), bRecursive ? TEXT("Yes") : TEXT("No")), true);

	return ChildActors;
}

TArray<AActor*> AMeshVertexSampler::GetChildActorsWithMeshes(AActor* ParentActor, bool bRecursive)
{
	TArray<AActor*> ActorsWithMeshes;

	if (!ParentActor)
	{
		LogError(TEXT("GetChildActorsWithMeshes: ParentActor is NULL!"));
		return ActorsWithMeshes;
	}

	// First get all child actors
	TArray<AActor*> AllChildren = GetChildActorsRecursive(ParentActor, bRecursive);

	// Also include the parent if it has meshes
	TArray<UStaticMeshComponent*> ParentMeshes;
	ParentActor->GetComponents<UStaticMeshComponent>(ParentMeshes);
	if (ParentMeshes.Num() > 0)
	{
		ActorsWithMeshes.Add(ParentActor);
	}

	// Filter to only those with mesh components
	for (AActor* Child : AllChildren)
	{
		if (Child)
		{
			TArray<UStaticMeshComponent*> MeshComponents;
			Child->GetComponents<UStaticMeshComponent>(MeshComponents);

			if (MeshComponents.Num() > 0)
			{
				ActorsWithMeshes.Add(Child);
			}
		}
	}

	LogMessage(FString::Printf(TEXT("Found %d actors with mesh components (out of %d total children)"), 
		ActorsWithMeshes.Num(), AllChildren.Num()), true);

	return ActorsWithMeshes;
}

TArray<AActor*> AMeshVertexSampler::GetAttachedActorsRecursive(AActor* ParentActor, bool bRecursive)
{
	TArray<AActor*> AttachedActors;

	if (!ParentActor)
	{
		LogError(TEXT("GetAttachedActorsRecursive: ParentActor is NULL!"));
		return AttachedActors;
	}

	// Use UE's built-in function with options
	ParentActor->GetAttachedActors(AttachedActors, true, bRecursive);

	LogMessage(FString::Printf(TEXT("Found %d attached actors to '%s'"), 
		AttachedActors.Num(), *ParentActor->GetName()), true);

	return AttachedActors;
}

int32 AMeshVertexSampler::CountChildActors(AActor* ParentActor, bool bRecursive)
{
	if (!ParentActor)
	{
		return 0;
	}

	TArray<AActor*> ChildActorList = GetChildActorsRecursive(ParentActor, bRecursive);
	
    LogMessage(FString::Printf(TEXT("Found %d clihd actors to '%s'"), 
	ChildActorList.Num(), *ParentActor->GetName()), true);

    return ChildActorList.Num();
}

FVertexSampleResult AMeshVertexSampler::SampleAndSpawnFromActorArray(
	const TArray<AActor*>& SourceActors,
	UHierarchicalInstancedStaticMeshComponent* TargetHISM,
	FSpawnConfiguration Config)
{
	FVertexSampleResult Result;

	LogMessage(TEXT("========================================"), true);
	LogMessage(FString::Printf(TEXT("Starting SampleAndSpawnFromActorArray with %d actors..."), SourceActors.Num()), true);

	if (SourceActors.Num() == 0)
	{
		LogWarning(TEXT("No source actors provided!"));
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

	// Get vertices from all actors
	TArray<FVector> AllVertices = GetVerticesFromActorArray(SourceActors, Config.VertexSkipCount);

	Result.TotalVerticesFound = AllVertices.Num() * Config.VertexSkipCount;
	Result.VerticesUsed = AllVertices.Num();
	Result.ComponentsProcessed = SourceActors.Num();

	if (AllVertices.Num() == 0)
	{
		LogWarning(TEXT("No vertices found from any actor!"));
		return Result;
	}

	// Spawn instances
	Result.InstancesSpawned = SpawnInstancesAtPositions(TargetHISM, AllVertices, Config);

	LogMessage(TEXT("========================================"), true);
	LogMessage(FString::Printf(TEXT("COMPLETED! Spawned %d instances from %d actors"), 
		Result.InstancesSpawned, SourceActors.Num()), true);

	return Result;
}

TArray<FVector> AMeshVertexSampler::GetVerticesFromActorArray(const TArray<AActor*>& SourceActors, int32 SkipCount)
{
	TArray<FVector> AllVertices;

	SkipCount = FMath::Max(1, SkipCount);

	LogMessage(FString::Printf(TEXT("Getting vertices from %d actors..."), SourceActors.Num()));

	int32 ActorIndex = 0;
	for (AActor* Actor : SourceActors)
	{
		if (!Actor)
		{
			continue;
		}

		TArray<FVector> ActorVertices = GetVerticesFromActor(Actor, SkipCount);
		AllVertices.Append(ActorVertices);

		if (bEnableVerboseLogging && ActorIndex < 10)
		{
			LogMessage(FString::Printf(TEXT("  Actor %d (%s): %d vertices"), 
				ActorIndex, *Actor->GetName(), ActorVertices.Num()));
		}

		ActorIndex++;
	}

	if (ActorIndex > 10)
	{
		LogMessage(FString::Printf(TEXT("  ... and %d more actors"), ActorIndex - 10));
	}

	LogMessage(FString::Printf(TEXT("Total vertices from all actors: %d"), AllVertices.Num()), true);

	return AllVertices;
}
