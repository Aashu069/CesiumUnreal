// CesiumShrinkWrap.cpp
// ShrinkWrap functionality for conforming meshes to Cesium terrain
// Ported concept from Unity Deora.Tools.ShrinkWrap for Unreal Engine

#include "CesiumShrinkWrap.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "KismetProceduralMeshLibrary.h"

// ============================================================================
// UCesiumShrinkWrapComponent Implementation
// ============================================================================

UCesiumShrinkWrapComponent::UCesiumShrinkWrapComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UCesiumShrinkWrapComponent::BeginPlay()
{
	Super::BeginPlay();
	Initialize();
}

void UCesiumShrinkWrapComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Debug visualization
	if (bEnableDebugVisualization)
	{
		DrawDebugVisualization();
	}

	// Auto-update if configured
	if (Config.UpdateFrequency > 0.0f)
	{
		TimeSinceLastUpdate += DeltaTime;
		if (TimeSinceLastUpdate >= Config.UpdateFrequency)
		{
			TimeSinceLastUpdate = 0.0f;
			Wrap();
		}
	}
}

void UCesiumShrinkWrapComponent::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	// Auto-detect target mesh component if not set
	if (!TargetMeshComponent)
	{
		AActor* Owner = GetOwner();
		if (Owner)
		{
			TargetMeshComponent = Owner->FindComponentByClass<UStaticMeshComponent>();
		}
	}

	if (!TargetMeshComponent)
	{
		LogError(TEXT("No target mesh component found! Please assign one."));
		return;
	}

	// Extract original mesh data
	if (!ExtractMeshData())
	{
		LogError(TEXT("Failed to extract mesh data from target component."));
		return;
	}

	// Create procedural mesh component for runtime modification
	ProceduralMesh = NewObject<UProceduralMeshComponent>(GetOwner(), NAME_None, RF_Transient);
	if (ProceduralMesh)
	{
		ProceduralMesh->RegisterComponent();
		ProceduralMesh->AttachToComponent(GetOwner()->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		ProceduralMesh->SetWorldTransform(TargetMeshComponent->GetComponentTransform());
		
		// Copy material from original mesh
		if (TargetMeshComponent->GetStaticMesh() && TargetMeshComponent->GetNumMaterials() > 0)
		{
			for (int32 i = 0; i < TargetMeshComponent->GetNumMaterials(); i++)
			{
				ProceduralMesh->SetMaterial(i, TargetMeshComponent->GetMaterial(i));
			}
		}

		// Initially hide procedural mesh until wrap is performed
		ProceduralMesh->SetVisibility(false);
	}

	bInitialized = true;
	Log(TEXT("ShrinkWrap component initialized successfully."), true);
	Log(FString::Printf(TEXT("  Vertices: %d, Triangles: %d"), OriginalVertices.Num(), OriginalIndices.Num() / 3));
}

bool UCesiumShrinkWrapComponent::ExtractMeshData()
{
	if (!TargetMeshComponent || !TargetMeshComponent->GetStaticMesh())
	{
		return false;
	}

	UStaticMesh* StaticMesh = TargetMeshComponent->GetStaticMesh();
	
	// Use Kismet library to get mesh section data
	UKismetProceduralMeshLibrary::GetSectionFromStaticMesh(
		StaticMesh,
		0,  // LOD index
		0,  // Section index
		OriginalVertices,
		OriginalIndices,
		OriginalNormals,
		OriginalUVs,
		OriginalTangents
	);

	// Get vertex colors if available
	OriginalVertexColors.SetNum(OriginalVertices.Num());
	for (int32 i = 0; i < OriginalVertexColors.Num(); i++)
	{
		OriginalVertexColors[i] = FColor::White;
	}

	// Try to get actual vertex colors
	if (StaticMesh->GetRenderData() && StaticMesh->GetRenderData()->LODResources.Num() > 0)
	{
		const FStaticMeshLODResources& LODResource = StaticMesh->GetRenderData()->LODResources[0];
		if (LODResource.bHasColorVertexData)
		{
			const FColorVertexBuffer& ColorBuffer = LODResource.VertexBuffers.ColorVertexBuffer;
			for (uint32 i = 0; i < ColorBuffer.GetNumVertices() && (int32)i < OriginalVertexColors.Num(); i++)
			{
				OriginalVertexColors[i] = ColorBuffer.VertexColor(i);
			}
		}
	}

	return OriginalVertices.Num() > 0;
}

FVector UCesiumShrinkWrapComponent::GetRaycastDirection() const
{
	// Get the owning actor's transform for local direction calculation
	FTransform ActorTransform = GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity;
	
	switch (Config.RaycastDirection)
	{
	case EShrinkWrapDirection::Down:
		return FVector(0, 0, -1);  // World down
	case EShrinkWrapDirection::Up:
		return FVector(0, 0, 1);   // World up
	case EShrinkWrapDirection::Forward:
		return ActorTransform.GetRotation().GetForwardVector();
	case EShrinkWrapDirection::Back:
		return -ActorTransform.GetRotation().GetForwardVector();
	case EShrinkWrapDirection::Right:
		return ActorTransform.GetRotation().GetRightVector();
	case EShrinkWrapDirection::Left:
		return -ActorTransform.GetRotation().GetRightVector();
	default:
		return FVector(0, 0, -1);
	}
}

bool UCesiumShrinkWrapComponent::RaycastVertex(const FVector& WorldPosition, const FVector& Direction, FVector& OutHitPoint) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = Config.bTraceComplex;
	QueryParams.bReturnPhysicalMaterial = false;  // Speed optimization
	QueryParams.AddIgnoredActor(GetOwner());  // Don't hit ourselves

	// Ignore the source mesh actor's components
	if (GetOwner())
	{
		QueryParams.AddIgnoredActor(GetOwner());
	}

	// Calculate ray start and end
	// Start slightly above the vertex position to handle cases where vertex is slightly inside terrain
	FVector RayStart = WorldPosition - Direction * 500.0f;  // Start 5m above (against direction)
	FVector RayEnd = WorldPosition + Direction * Config.RaycastDistance;

	bool bHit = false;

	if (TerrainActors.Num() > 0)
	{
		// Trace against specific actors
		for (AActor* TerrainActor : TerrainActors)
		{
			if (!TerrainActor) continue;

			TArray<UPrimitiveComponent*> Components;
			TerrainActor->GetComponents<UPrimitiveComponent>(Components);

			for (UPrimitiveComponent* Comp : Components)
			{
				if (Comp && Comp->LineTraceComponent(HitResult, RayStart, RayEnd, QueryParams))
				{
					OutHitPoint = HitResult.ImpactPoint;
					bHit = true;
					break;
				}
			}
			if (bHit) break;
		}
	}
	else
	{
		// Trace against world using collision channel
		bHit = World->LineTraceSingleByChannel(
			HitResult,
			RayStart,
			RayEnd,
			Config.CollisionChannel,
			QueryParams
		);

		if (bHit)
		{
			OutHitPoint = HitResult.ImpactPoint;
		}
	}

	// Bidirectional check - if we missed, try the opposite direction
	if (!bHit && Config.bBidirectionalCheck)
	{
		FVector OppositeDirection = -Direction;
		RayStart = WorldPosition - OppositeDirection * 500.0f;
		RayEnd = WorldPosition + OppositeDirection * Config.RaycastDistance;

		if (TerrainActors.Num() > 0)
		{
			for (AActor* TerrainActor : TerrainActors)
			{
				if (!TerrainActor) continue;

				TArray<UPrimitiveComponent*> Components;
				TerrainActor->GetComponents<UPrimitiveComponent>(Components);

				for (UPrimitiveComponent* Comp : Components)
				{
					if (Comp && Comp->LineTraceComponent(HitResult, RayStart, RayEnd, QueryParams))
					{
						OutHitPoint = HitResult.ImpactPoint;
						bHit = true;
						break;
					}
				}
				if (bHit) break;
			}
		}
		else
		{
			bHit = World->LineTraceSingleByChannel(
				HitResult,
				RayStart,
				RayEnd,
				Config.CollisionChannel,
				QueryParams
			);

			if (bHit)
			{
				OutHitPoint = HitResult.ImpactPoint;
			}
		}
	}

	return bHit;
}

FShrinkWrapResult UCesiumShrinkWrapComponent::Wrap()
{
	FShrinkWrapResult Result;

	if (!bInitialized)
	{
		Initialize();
	}

	if (!bInitialized || OriginalVertices.Num() == 0)
	{
		LogError(TEXT("Cannot wrap - not initialized or no vertices."));
		return Result;
	}

	if (bIsWrapping)
	{
		LogWarning(TEXT("Wrap already in progress, skipping."));
		return Result;
	}

	bIsWrapping = true;
	Log(TEXT("Starting shrink wrap operation..."), true);

	FTransform MeshTransform = TargetMeshComponent->GetComponentTransform();
	FVector RayDirection = GetRaycastDirection();

	TArray<FVector> NewVertices;
	NewVertices.SetNum(OriginalVertices.Num());
	DebugPoints.Empty();
	DebugPoints.Reserve(OriginalVertices.Num());

	Result.TotalVertices = OriginalVertices.Num();

	for (int32 i = 0; i < OriginalVertices.Num(); i++)
	{
		// Transform vertex to world space
		FVector WorldPosition = MeshTransform.TransformPosition(OriginalVertices[i]);

		FWrapDebugPoint DebugPoint;
		DebugPoint.OriginalPosition = WorldPosition;

		FVector HitPoint;
		if (RaycastVertex(WorldPosition, RayDirection, HitPoint))
		{
			// Apply offset in the opposite direction of the ray (above terrain)
			FVector OffsetVector = -RayDirection * Config.Offset;
			FVector FinalWorldPosition = HitPoint + OffsetVector;

			// Transform back to local space
			NewVertices[i] = MeshTransform.InverseTransformPosition(FinalWorldPosition);

			DebugPoint.TargetPosition = FinalWorldPosition;
			DebugPoint.bHit = true;
			Result.VerticesWrapped++;
		}
		else
		{
			// Keep original position if no hit
			NewVertices[i] = OriginalVertices[i];
			DebugPoint.TargetPosition = WorldPosition;
			DebugPoint.bHit = false;
			Result.VerticesMissed++;
		}

		DebugPoints.Add(DebugPoint);
	}

	// Apply wrapped vertices
	ApplyWrappedVertices(NewVertices);

	bIsWrapped = true;
	bIsWrapping = false;
	Result.bSuccess = true;

	Log(FString::Printf(TEXT("Wrap complete! Wrapped: %d, Missed: %d"), 
		Result.VerticesWrapped, Result.VerticesMissed), true);

	return Result;
}

void UCesiumShrinkWrapComponent::ApplyWrappedVertices(const TArray<FVector>& NewVertices)
{
	if (!ProceduralMesh || NewVertices.Num() != OriginalVertices.Num())
	{
		return;
	}

	// Create procedural mesh section with new vertices
	ProceduralMesh->CreateMeshSection_LinearColor(
		0,
		NewVertices,
		OriginalIndices,
		OriginalNormals,
		OriginalUVs,
		TArray<FLinearColor>(),  // Linear colors
		OriginalTangents,
		true  // Create collision
	);

	// Show procedural mesh and hide original
	ProceduralMesh->SetVisibility(true);
	if (TargetMeshComponent)
	{
		TargetMeshComponent->SetVisibility(false);
	}
}

void UCesiumShrinkWrapComponent::ResetWrap()
{
	if (!bInitialized)
	{
		return;
	}

	// Clear procedural mesh
	if (ProceduralMesh)
	{
		ProceduralMesh->ClearAllMeshSections();
		ProceduralMesh->SetVisibility(false);
	}

	// Show original mesh
	if (TargetMeshComponent)
	{
		TargetMeshComponent->SetVisibility(true);
	}

	bIsWrapped = false;
	DebugPoints.Empty();

	Log(TEXT("Wrap reset - original mesh restored."), true);
}

void UCesiumShrinkWrapComponent::PreviewWrap()
{
	if (!bInitialized)
	{
		Initialize();
	}

	if (!bInitialized)
	{
		return;
	}

	FTransform MeshTransform = TargetMeshComponent->GetComponentTransform();
	FVector RayDirection = GetRaycastDirection();

	DebugPoints.Empty();
	DebugPoints.Reserve(OriginalVertices.Num());

	for (int32 i = 0; i < OriginalVertices.Num(); i++)
	{
		FVector WorldPosition = MeshTransform.TransformPosition(OriginalVertices[i]);

		FWrapDebugPoint DebugPoint;
		DebugPoint.OriginalPosition = WorldPosition;

		FVector HitPoint;
		if (RaycastVertex(WorldPosition, RayDirection, HitPoint))
		{
			DebugPoint.TargetPosition = HitPoint + (-RayDirection * Config.Offset);
			DebugPoint.bHit = true;
		}
		else
		{
			DebugPoint.TargetPosition = WorldPosition;
			DebugPoint.bHit = false;
		}

		DebugPoints.Add(DebugPoint);
	}

	Log(TEXT("Preview generated - enable debug visualization to see results."), true);
}

FString UCesiumShrinkWrapComponent::GetWrapInfo() const
{
	return FString::Printf(TEXT("Vertices: %d | Wrapped: %s | Debug Points: %d"),
		OriginalVertices.Num(),
		bIsWrapped ? TEXT("Yes") : TEXT("No"),
		DebugPoints.Num());
}

void UCesiumShrinkWrapComponent::DrawDebugVisualization()
{
	UWorld* World = GetWorld();
	if (!World || DebugPoints.Num() == 0)
	{
		return;
	}

	for (const FWrapDebugPoint& Point : DebugPoints)
	{
		FColor Color = Point.bHit ? FColor::Green : FColor::Red;
		
		// Draw original position
		DrawDebugPoint(World, Point.OriginalPosition, 5.0f, FColor::Blue, false, 0.0f);
		
		// Draw target position
		DrawDebugPoint(World, Point.TargetPosition, 5.0f, Color, false, 0.0f);
		
		// Draw line between them
		DrawDebugLine(World, Point.OriginalPosition, Point.TargetPosition, Color, false, 0.0f, 0, 1.0f);
	}
}

void UCesiumShrinkWrapComponent::Log(const FString& Message, bool bForce) const
{
	if (bEnableVerboseLogging || bForce)
	{
		UE_LOG(LogTemp, Log, TEXT("[ShrinkWrap] %s"), *Message);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, FString::Printf(TEXT("[ShrinkWrap] %s"), *Message));
		}
	}
}

void UCesiumShrinkWrapComponent::LogWarning(const FString& Message) const
{
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] %s"), *Message);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, FString::Printf(TEXT("[ShrinkWrap WARNING] %s"), *Message));
	}
}

void UCesiumShrinkWrapComponent::LogError(const FString& Message) const
{
	UE_LOG(LogTemp, Error, TEXT("[ShrinkWrap] %s"), *Message);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, FString::Printf(TEXT("[ShrinkWrap ERROR] %s"), *Message));
	}
}

// ============================================================================
// ACesiumShrinkWrapActor Implementation
// ============================================================================

ACesiumShrinkWrapActor::ACesiumShrinkWrapActor()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ACesiumShrinkWrapActor::BeginPlay()
{
	Super::BeginPlay();

	if (bWrapOnBeginPlay && SourceMeshActor)
	{
		// Delay wrap to allow Cesium tiles to load their collision
		FTimerHandle TimerHandle;
		float Delay = FMath::Max(0.1f, Config.InitialDelay);
		GetWorldTimerManager().SetTimer(TimerHandle, [this]()
		{
			Log(TEXT("Initial delay complete, starting wrap operation..."), true);
			WrapAllMeshes();
		}, Delay, false);
		
		Log(FString::Printf(TEXT("ShrinkWrap will execute in %.1f seconds (waiting for terrain to load)"), Delay), true);
	}
}

void ACesiumShrinkWrapActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Debug visualization
	if (bEnableDebugVisualization)
	{
		DrawDebugVisualization();
	}

	// Auto-update
	if (Config.UpdateFrequency > 0.0f && bIsWrapped)
	{
		TimeSinceLastUpdate += DeltaTime;
		if (TimeSinceLastUpdate >= Config.UpdateFrequency)
		{
			TimeSinceLastUpdate = 0.0f;
			WrapAllMeshes();
		}
	}
}

FVector ACesiumShrinkWrapActor::GetRaycastDirection() const
{
	switch (Config.RaycastDirection)
	{
	case EShrinkWrapDirection::Down:
		return FVector(0, 0, -1);
	case EShrinkWrapDirection::Up:
		return FVector(0, 0, 1);
	case EShrinkWrapDirection::Forward:
		return GetActorForwardVector();
	case EShrinkWrapDirection::Back:
		return -GetActorForwardVector();
	case EShrinkWrapDirection::Right:
		return GetActorRightVector();
	case EShrinkWrapDirection::Left:
		return -GetActorRightVector();
	default:
		return FVector(0, 0, -1);
	}
}

bool ACesiumShrinkWrapActor::RaycastVertex(const FVector& WorldPosition, const FVector& Direction, FVector& OutHitPoint) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = Config.bTraceComplex;
	QueryParams.bReturnPhysicalMaterial = false;  // Speed optimization
	QueryParams.AddIgnoredActor(this);
	
	if (SourceMeshActor)
	{
		QueryParams.AddIgnoredActor(SourceMeshActor);
	}

	FVector RayStart = WorldPosition - Direction * 500.0f;
	FVector RayEnd = WorldPosition + Direction * Config.RaycastDistance;

	bool bHit = false;

	if (TerrainActors.Num() > 0)
	{
		for (AActor* TerrainActor : TerrainActors)
		{
			if (!TerrainActor) continue;

			TArray<UPrimitiveComponent*> Components;
			TerrainActor->GetComponents<UPrimitiveComponent>(Components);

			for (UPrimitiveComponent* Comp : Components)
			{
				if (Comp && Comp->LineTraceComponent(HitResult, RayStart, RayEnd, QueryParams))
				{
					OutHitPoint = HitResult.ImpactPoint;
					bHit = true;
					break;
				}
			}
			if (bHit) break;
		}
	}
	else
	{
		bHit = World->LineTraceSingleByChannel(
			HitResult,
			RayStart,
			RayEnd,
			Config.CollisionChannel,
			QueryParams
		);

		if (bHit)
		{
			OutHitPoint = HitResult.ImpactPoint;
		}
	}

	// Bidirectional check
	if (!bHit && Config.bBidirectionalCheck)
	{
		FVector OppositeDirection = -Direction;
		RayStart = WorldPosition - OppositeDirection * 500.0f;
		RayEnd = WorldPosition + OppositeDirection * Config.RaycastDistance;

		bHit = World->LineTraceSingleByChannel(
			HitResult,
			RayStart,
			RayEnd,
			Config.CollisionChannel,
			QueryParams
		);

		if (bHit)
		{
			OutHitPoint = HitResult.ImpactPoint;
		}
	}

	return bHit;
}

FShrinkWrapResult ACesiumShrinkWrapActor::WrapAllMeshes()
{
	FShrinkWrapResult TotalResult;

	if (!SourceMeshActor)
	{
		LogError(TEXT("No source mesh actor assigned!"));
		return TotalResult;
	}

	if (bIsWrapping)
	{
		LogWarning(TEXT("Wrap operation already in progress."));
		return TotalResult;
	}

	bIsWrapping = true;
	Log(TEXT("Starting wrap for all mesh components..."), true);

	// Get all static mesh components from source actor
	TArray<UStaticMeshComponent*> MeshComponents;
	SourceMeshActor->GetComponents<UStaticMeshComponent>(MeshComponents);

	Log(FString::Printf(TEXT("Found %d mesh components to wrap."), MeshComponents.Num()));

	for (UStaticMeshComponent* MeshComp : MeshComponents)
	{
		if (!MeshComp || !MeshComp->GetStaticMesh() || !MeshComp->IsVisible())
		{
			continue;
		}

		FShrinkWrapResult CompResult = WrapMeshComponent(MeshComp);
		TotalResult.TotalVertices += CompResult.TotalVertices;
		TotalResult.VerticesWrapped += CompResult.VerticesWrapped;
		TotalResult.VerticesMissed += CompResult.VerticesMissed;
	}

	bIsWrapped = true;
	bIsWrapping = false;
	TotalResult.bSuccess = true;

	Log(FString::Printf(TEXT("Wrap complete! Total wrapped: %d, missed: %d"), 
		TotalResult.VerticesWrapped, TotalResult.VerticesMissed), true);

	return TotalResult;
}

FShrinkWrapResult ACesiumShrinkWrapActor::WrapMeshComponent(UStaticMeshComponent* MeshComponent)
{
	FShrinkWrapResult Result;

	if (!MeshComponent || !MeshComponent->GetStaticMesh())
	{
		return Result;
	}

	UStaticMesh* StaticMesh = MeshComponent->GetStaticMesh();

	// Extract mesh data
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;

	UKismetProceduralMeshLibrary::GetSectionFromStaticMesh(
		StaticMesh, 0, 0, Vertices, Indices, Normals, UVs, Tangents
	);

	if (Vertices.Num() == 0)
	{
		Log(FString::Printf(TEXT("Skipping component %s - no vertices."), *MeshComponent->GetName()));
		return Result;
	}

	Result.TotalVertices = Vertices.Num();

	FTransform MeshTransform = MeshComponent->GetComponentTransform();
	FVector RayDirection = GetRaycastDirection();

	TArray<FVector> NewVertices;
	NewVertices.SetNum(Vertices.Num());

	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		FVector WorldPosition = MeshTransform.TransformPosition(Vertices[i]);

		FWrapDebugPoint DebugPoint;
		DebugPoint.OriginalPosition = WorldPosition;

		FVector HitPoint;
		if (RaycastVertex(WorldPosition, RayDirection, HitPoint))
		{
			FVector OffsetVector = -RayDirection * Config.Offset;
			FVector FinalWorldPosition = HitPoint + OffsetVector;
			NewVertices[i] = MeshTransform.InverseTransformPosition(FinalWorldPosition);

			DebugPoint.TargetPosition = FinalWorldPosition;
			DebugPoint.bHit = true;
			Result.VerticesWrapped++;
		}
		else
		{
			NewVertices[i] = Vertices[i];
			DebugPoint.TargetPosition = WorldPosition;
			DebugPoint.bHit = false;
			Result.VerticesMissed++;
		}

		DebugPoints.Add(DebugPoint);
	}

	// Create procedural mesh
	UProceduralMeshComponent* ProcMesh = NewObject<UProceduralMeshComponent>(this, NAME_None, RF_Transient);
	if (ProcMesh)
	{
		ProcMesh->RegisterComponent();
		ProcMesh->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		ProcMesh->SetWorldTransform(MeshTransform);

		// Copy materials
		for (int32 i = 0; i < MeshComponent->GetNumMaterials(); i++)
		{
			ProcMesh->SetMaterial(i, MeshComponent->GetMaterial(i));
		}

		// Create mesh section
		ProcMesh->CreateMeshSection_LinearColor(
			0,
			NewVertices,
			Indices,
			Normals,
			UVs,
			TArray<FLinearColor>(),
			Tangents,
			true
		);

		CreatedProceduralMeshes.Add(ProcMesh);
		
		// Hide original
		MeshComponent->SetVisibility(false);
		HiddenOriginalMeshes.Add(MeshComponent);
	}

	Result.bSuccess = true;
	return Result;
}

void ACesiumShrinkWrapActor::ResetAllWraps()
{
	// Destroy procedural meshes
	for (UProceduralMeshComponent* ProcMesh : CreatedProceduralMeshes)
	{
		if (ProcMesh)
		{
			ProcMesh->DestroyComponent();
		}
	}
	CreatedProceduralMeshes.Empty();

	// Show original meshes
	for (UStaticMeshComponent* MeshComp : HiddenOriginalMeshes)
	{
		if (MeshComp)
		{
			MeshComp->SetVisibility(true);
		}
	}
	HiddenOriginalMeshes.Empty();

	bIsWrapped = false;
	DebugPoints.Empty();

	Log(TEXT("All wraps reset - original meshes restored."), true);
}

void ACesiumShrinkWrapActor::DrawDebugVisualization()
{
	UWorld* World = GetWorld();
	if (!World || DebugPoints.Num() == 0)
	{
		return;
	}

	// Only draw a subset to avoid performance issues
	int32 Step = FMath::Max(1, DebugPoints.Num() / 1000);
	
	for (int32 i = 0; i < DebugPoints.Num(); i += Step)
	{
		const FWrapDebugPoint& Point = DebugPoints[i];
		FColor Color = Point.bHit ? FColor::Green : FColor::Red;
		
		DrawDebugPoint(World, Point.OriginalPosition, 5.0f, FColor::Blue, false, 0.0f);
		DrawDebugPoint(World, Point.TargetPosition, 5.0f, Color, false, 0.0f);
		DrawDebugLine(World, Point.OriginalPosition, Point.TargetPosition, Color, false, 0.0f, 0, 1.0f);
	}
}

void ACesiumShrinkWrapActor::Log(const FString& Message, bool bForce) const
{
	if (bEnableVerboseLogging || bForce)
	{
		UE_LOG(LogTemp, Log, TEXT("[ShrinkWrapActor] %s"), *Message);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, FString::Printf(TEXT("[ShrinkWrap] %s"), *Message));
		}
	}
}

void ACesiumShrinkWrapActor::LogWarning(const FString& Message) const
{
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrapActor] %s"), *Message);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, FString::Printf(TEXT("[ShrinkWrap WARNING] %s"), *Message));
	}
}

void ACesiumShrinkWrapActor::LogError(const FString& Message) const
{
	UE_LOG(LogTemp, Error, TEXT("[ShrinkWrapActor] %s"), *Message);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, FString::Printf(TEXT("[ShrinkWrap ERROR] %s"), *Message));
	}
}
