// CesiumShrinkWrap.cpp
// ShrinkWrap functionality for conforming meshes to Cesium terrain
// Ported concept from Unity Deora.Tools.ShrinkWrap for Unreal Engine
// Updated to support Cesium 3D Tilesets using ICesiumPrimitive interface

// NOMINMAX MUST be defined before ANY includes to prevent windows.h min/max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CesiumShrinkWrap.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "KismetProceduralMeshLibrary.h"
#include "EngineUtils.h"  // For TActorIterator
#include "Kismet/GameplayStatics.h"  // For UGameplayStatics::GetAllActorsWithTag

#include <variant>  // For std::visit on IndexAccessor variant type

// Include Cesium private header to access PositionAccessor (CPU-safe vertex data)
// This is necessary for packaged builds where GPU vertex buffers are not accessible
THIRD_PARTY_INCLUDES_START
#include "CesiumRuntime/Private/CesiumPrimitive.h"
THIRD_PARTY_INCLUDES_END

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

	// Always log to verify the actor is alive and BeginPlay was called
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] BeginPlay called. Actor: %s"), *GetName());
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] bWrapOnBeginPlay: %s, SourceMeshActor: %s"), 
		bWrapOnBeginPlay ? TEXT("true") : TEXT("false"),
		SourceMeshActor ? *SourceMeshActor->GetName() : TEXT("NULL"));

	if (!bWrapOnBeginPlay)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] bWrapOnBeginPlay is false, skipping auto-wrap"));
		return;
	}

	// Try to find SourceMeshActor if it's null (common issue in packaged builds)
	if (!SourceMeshActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] SourceMeshActor is NULL, attempting fallback lookup..."));
		
		// Try by name first
		if (!SourceMeshActorName.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Searching for actor by name: %s"), *SourceMeshActorName);
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				if (It->GetName().Contains(SourceMeshActorName))
				{
					SourceMeshActor = *It;
					UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Found actor by name: %s"), *SourceMeshActor->GetName());
					break;
				}
			}
		}
		
		// Try by tag if still null
		if (!SourceMeshActor && !SourceMeshActorTag.IsNone())
		{
			UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Searching for actor by tag: %s"), *SourceMeshActorTag.ToString());
			TArray<AActor*> TaggedActors;
			UGameplayStatics::GetAllActorsWithTag(GetWorld(), SourceMeshActorTag, TaggedActors);
			if (TaggedActors.Num() > 0)
			{
				SourceMeshActor = TaggedActors[0];
				UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Found actor by tag: %s"), *SourceMeshActor->GetName());
			}
		}
		
		// Final check
		if (!SourceMeshActor)
		{
			UE_LOG(LogTemp, Error, TEXT("[ShrinkWrap] Could not find SourceMeshActor! Set SourceMeshActorName or SourceMeshActorTag."));
			
			// Log all actors in scene to help debug
			UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Available actors in scene:"));
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap]   - %s (Class: %s)"), *It->GetName(), *It->GetClass()->GetName());
			}
			return;
		}
	}

	// Delay wrap to allow Cesium tiles to load their collision
	FTimerHandle TimerHandle;
	float Delay = FMath::Max(0.1f, Config.InitialDelay);
	GetWorldTimerManager().SetTimer(TimerHandle, [this]()
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Initial delay complete, starting wrap operation (Attempt %d)..."), CurrentRetryAttempt + 1);
		Log(TEXT("Initial delay complete, starting wrap operation..."), true);
		ExecuteWrapWithRetry();
	}, Delay, false);
	
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Will execute in %.1f seconds (waiting for terrain to load)"), Delay);
	Log(FString::Printf(TEXT("ShrinkWrap will execute in %.1f seconds (waiting for terrain to load)"), Delay), true);
}

void ACesiumShrinkWrapActor::ExecuteWrapWithRetry()
{
	// Reset procedural meshes before retry
	if (CurrentRetryAttempt > 0)
	{
		ResetAllWraps();
	}
	
	CurrentRetryAttempt++;
	
	// Execute wrap
	FShrinkWrapResult Result = WrapAllMeshes();
	
	// Calculate hit ratio
	if (Result.TotalVertices > 0)
	{
		LastHitRatio = static_cast<float>(Result.VerticesWrapped) / static_cast<float>(Result.TotalVertices);
	}
	else
	{
		LastHitRatio = 0.0f;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Attempt %d: Hit ratio = %.1f%% (threshold = %.1f%%)"), 
		CurrentRetryAttempt, LastHitRatio * 100.0f, Config.RetryHitRatioThreshold * 100.0f);
	
	// Check if we need to retry
	bool bNeedsRetry = Config.bAutoRetryOnLowHitRate && 
					   LastHitRatio < Config.RetryHitRatioThreshold && 
					   CurrentRetryAttempt < Config.MaxRetryAttempts;
	
	if (bNeedsRetry)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Hit ratio too low, scheduling retry %d/%d in %.1f seconds..."), 
			CurrentRetryAttempt + 1, Config.MaxRetryAttempts, Config.RetryInterval);
		Log(FString::Printf(TEXT("Terrain not loaded yet, retrying in %.1f seconds... (%d/%d)"), 
			Config.RetryInterval, CurrentRetryAttempt + 1, Config.MaxRetryAttempts), true);
		
		// Schedule retry
		FTimerHandle RetryTimerHandle;
		GetWorldTimerManager().SetTimer(RetryTimerHandle, [this]()
		{
			ExecuteWrapWithRetry();
		}, Config.RetryInterval, false);
	}
	else if (LastHitRatio >= Config.RetryHitRatioThreshold)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] SUCCESS! Hit ratio %.1f%% meets threshold. Wrap complete."), 
			LastHitRatio * 100.0f);
		Log(FString::Printf(TEXT("Wrap successful! Hit ratio: %.1f%%"), LastHitRatio * 100.0f), true);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Max retries (%d) reached. Final hit ratio: %.1f%%"), 
			Config.MaxRetryAttempts, LastHitRatio * 100.0f);
		Log(FString::Printf(TEXT("Max retries reached. Final hit ratio: %.1f%%"), LastHitRatio * 100.0f), true);
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

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] WrapAllMeshes called"));

	if (!SourceMeshActor)
	{
		UE_LOG(LogTemp, Error, TEXT("[ShrinkWrap] No source mesh actor assigned!"));
		LogError(TEXT("No source mesh actor assigned!"));
		return TotalResult;
	}

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] SourceMeshActor: %s (Class: %s)"), 
		*SourceMeshActor->GetName(), *SourceMeshActor->GetClass()->GetName());

	if (bIsWrapping)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Wrap operation already in progress."));
		LogWarning(TEXT("Wrap operation already in progress."));
		return TotalResult;
	}

	bIsWrapping = true;
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Starting wrap for all mesh components..."));
	Log(TEXT("Starting wrap for all mesh components..."), true);

	// Get ALL primitive components - this includes both StaticMeshComponent AND CesiumGltfPrimitiveComponent
	TArray<UPrimitiveComponent*> AllPrimitives;
	SourceMeshActor->GetComponents<UPrimitiveComponent>(AllPrimitives);

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Found %d primitive components total."), AllPrimitives.Num());

	int32 ProcessedCount = 0;
	int32 CesiumCount = 0;
	int32 StaticMeshCount = 0;

	for (UPrimitiveComponent* PrimComp : AllPrimitives)
	{
		if (!PrimComp)
		{
			continue;
		}
		
		// Log what we're checking
		UE_LOG(LogTemp, Log, TEXT("[ShrinkWrap] Checking component: %s (Class: %s, Visible: %d)"), 
			*PrimComp->GetName(), *PrimComp->GetClass()->GetName(), PrimComp->IsVisible());

		// Check if this is a Cesium primitive (CesiumGltfPrimitiveComponent)
		ICesiumPrimitive* CesiumPrimitive = Cast<ICesiumPrimitive>(PrimComp);
		if (CesiumPrimitive)
		{
			CesiumCount++;
			FShrinkWrapResult CompResult = WrapCesiumPrimitive(PrimComp, CesiumPrimitive);
			TotalResult.TotalVertices += CompResult.TotalVertices;
			TotalResult.VerticesWrapped += CompResult.VerticesWrapped;
			TotalResult.VerticesMissed += CompResult.VerticesMissed;
			if (CompResult.bSuccess) ProcessedCount++;
		}
		else if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(PrimComp))
		{
			// Regular StaticMeshComponent
			if (StaticMeshComp->GetStaticMesh())
			{
				StaticMeshCount++;
				FShrinkWrapResult CompResult = WrapMeshComponent(StaticMeshComp);
				TotalResult.TotalVertices += CompResult.TotalVertices;
				TotalResult.VerticesWrapped += CompResult.VerticesWrapped;
				TotalResult.VerticesMissed += CompResult.VerticesMissed;
				if (CompResult.bSuccess) ProcessedCount++;
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Component breakdown: Cesium=%d, StaticMesh=%d, Processed=%d"), 
		CesiumCount, StaticMeshCount, ProcessedCount);

	bIsWrapped = true;
	bIsWrapping = false;
	TotalResult.bSuccess = ProcessedCount > 0;

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Wrap complete! Total wrapped: %d, missed: %d"), 
		TotalResult.VerticesWrapped, TotalResult.VerticesMissed);
	Log(FString::Printf(TEXT("Wrap complete! Total wrapped: %d, missed: %d"), 
		TotalResult.VerticesWrapped, TotalResult.VerticesMissed), true);

	return TotalResult;
}

FShrinkWrapResult ACesiumShrinkWrapActor::WrapMeshComponent(UStaticMeshComponent* MeshComponent)
{
	FShrinkWrapResult Result;

	if (!MeshComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] WrapMeshComponent - MeshComponent is NULL"));
		return Result;
	}

	if (!MeshComponent->GetStaticMesh())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] WrapMeshComponent - Component %s has no StaticMesh"), *MeshComponent->GetName());
		return Result;
	}

	UStaticMesh* StaticMesh = MeshComponent->GetStaticMesh();
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Processing mesh component: %s, StaticMesh: %s"), 
		*MeshComponent->GetName(), *StaticMesh->GetName());

	// Check if CPU access is available (critical for packaged builds!)
	bool bHasCPUAccess = StaticMesh->bAllowCPUAccess;
	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Mesh %s - AllowCPUAccess: %s"), 
		*StaticMesh->GetName(), bHasCPUAccess ? TEXT("YES") : TEXT("NO"));

	// Extract mesh data
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;

	UKismetProceduralMeshLibrary::GetSectionFromStaticMesh(
		StaticMesh, 0, 0, Vertices, Indices, Normals, UVs, Tangents
	);

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] GetSectionFromStaticMesh result: %d vertices, %d indices"), 
		Vertices.Num(), Indices.Num());

	if (Vertices.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[ShrinkWrap] Skipping component %s - no vertices extracted! CPU Access may be disabled."), 
			*MeshComponent->GetName());
		Log(FString::Printf(TEXT("Skipping component %s - no vertices."), *MeshComponent->GetName()));
		return Result;
	}

	Result.TotalVertices = Vertices.Num();

	FTransform MeshTransform = MeshComponent->GetComponentTransform();
	FVector RayDirection = GetRaycastDirection();

	TArray<FVector> NewVertices;
	NewVertices.SetNum(Vertices.Num());

	// First pass: raycast all vertices and track which ones hit
	TArray<bool> VertexHit;
	VertexHit.SetNum(Vertices.Num());
	double TotalWrappedZ = 0.0;
	int32 WrappedCount = 0;

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
			VertexHit[i] = true;
			TotalWrappedZ += FinalWorldPosition.Z;
			WrappedCount++;
			Result.VerticesWrapped++;
		}
		else
		{
			// Temporarily store original - we'll fix missed vertices in second pass
			NewVertices[i] = Vertices[i];
			DebugPoint.TargetPosition = WorldPosition;
			DebugPoint.bHit = false;
			VertexHit[i] = false;
			Result.VerticesMissed++;
		}

		DebugPoints.Add(DebugPoint);
	}

	// Skip this mesh if no vertices were wrapped and option is enabled
	if (Config.bSkipFullyMissedMeshes && WrappedCount == 0)
	{
		Log(FString::Printf(TEXT("Skipping component %s - no terrain hits (keeping original visible)."), *MeshComponent->GetName()));
		Result.bSuccess = false;
		return Result;
	}

	// Check minimum hit ratio
	float HitRatio = (float)WrappedCount / (float)Vertices.Num();
	if (HitRatio < Config.MinHitRatio)
	{
		Log(FString::Printf(TEXT("Skipping component %s - hit ratio %.1f%% below threshold %.1f%%."), 
			*MeshComponent->GetName(), HitRatio * 100.0f, Config.MinHitRatio * 100.0f));
		Result.bSuccess = false;
		return Result;
	}

	// Pre-compute wrapped vertex world positions for IDW lookup (only if using IDW)
	TArray<FVector> WrappedWorldPositions;
	TArray<int32> WrappedIndices;
	if (Config.MissedVertexHandling == 3 && Result.VerticesMissed > 0)
	{
		WrappedWorldPositions.Reserve(WrappedCount);
		WrappedIndices.Reserve(WrappedCount);
		for (int32 i = 0; i < Vertices.Num(); i++)
		{
			if (VertexHit[i])
			{
				WrappedWorldPositions.Add(MeshTransform.TransformPosition(NewVertices[i]));
				WrappedIndices.Add(i);
			}
		}
	}

	// Second pass: handle missed vertices based on config
	if (Result.VerticesMissed > 0 && WrappedCount > 0)
	{
		double AverageWrappedZ = TotalWrappedZ / WrappedCount;
		const float SearchRadiusSq = Config.NeighborSearchRadius * Config.NeighborSearchRadius;

		for (int32 i = 0; i < Vertices.Num(); i++)
		{
			if (!VertexHit[i])
			{
				FVector WorldPosition = MeshTransform.TransformPosition(Vertices[i]);
				FVector AdjustedPosition = WorldPosition;

				switch (Config.MissedVertexHandling)
				{
				case 0:  // Keep original
					// Already set, do nothing
					break;

				case 1:  // Use average Z of wrapped vertices
					AdjustedPosition.Z = AverageWrappedZ;
					NewVertices[i] = MeshTransform.InverseTransformPosition(AdjustedPosition);
					break;

				case 2:  // Use fallback height offset
					AdjustedPosition.Z += Config.FallbackHeightOffset;
					NewVertices[i] = MeshTransform.InverseTransformPosition(AdjustedPosition);
					break;

				case 3:  // IDW (Inverse Distance Weighted) interpolation from neighbors
				{
					// Shepard's method: weighted average based on inverse distance
					// w_i = 1 / d^p where p is the power parameter (typically 2)
					double WeightedSum = 0.0;
					double TotalWeight = 0.0;
					int32 NeighborsUsed = 0;

					// Only use XY distance (2D) since we're interpolating Z
					FVector2D MissedXY(WorldPosition.X, WorldPosition.Y);

					for (int32 j = 0; j < WrappedWorldPositions.Num(); j++)
					{
						const FVector& NeighborPos = WrappedWorldPositions[j];
						FVector2D NeighborXY(NeighborPos.X, NeighborPos.Y);
						
						float DistSq = FVector2D::DistSquared(MissedXY, NeighborXY);
						
						// Skip if outside search radius
						if (DistSq > SearchRadiusSq)
							continue;
						
						// Avoid division by zero
						if (DistSq < 1.0f)
							DistSq = 1.0f;
						
						// Shepard's IDW weight: w = 1/d^p
						// Using DistSq, so for p=2: w = 1/distSq
						// For p=1: w = 1/sqrt(distSq), for p=3: w = 1/(distSq * sqrt(distSq))
						double Dist = FMath::Sqrt(DistSq);
						double Weight = 1.0 / FMath::Pow(Dist, Config.IDWPower);
						
						WeightedSum += NeighborPos.Z * Weight;
						TotalWeight += Weight;
						NeighborsUsed++;
					}

					if (TotalWeight > 0.0 && NeighborsUsed >= 1)
					{
						// Apply IDW interpolated Z
						AdjustedPosition.Z = WeightedSum / TotalWeight;
						NewVertices[i] = MeshTransform.InverseTransformPosition(AdjustedPosition);
					}
					else
					{
						// Fallback to global average if no neighbors found
						AdjustedPosition.Z = AverageWrappedZ;
						NewVertices[i] = MeshTransform.InverseTransformPosition(AdjustedPosition);
					}
					break;
				}
				} // end switch
			}
		}
	}

	// Optional smoothing pass to reduce jagged edges
	if (Config.bSmoothResults && Config.SmoothingIterations > 0)
	{
		// Build adjacency from triangles
		TMap<int32, TArray<int32>> VertexNeighbors;
		for (int32 t = 0; t < Indices.Num(); t += 3)
		{
			if (t + 2 >= Indices.Num()) break;
			int32 I0 = Indices[t];
			int32 I1 = Indices[t + 1];
			int32 I2 = Indices[t + 2];
			
			if (I0 < NewVertices.Num() && I1 < NewVertices.Num() && I2 < NewVertices.Num())
			{
				VertexNeighbors.FindOrAdd(I0).AddUnique(I1);
				VertexNeighbors.FindOrAdd(I0).AddUnique(I2);
				VertexNeighbors.FindOrAdd(I1).AddUnique(I0);
				VertexNeighbors.FindOrAdd(I1).AddUnique(I2);
				VertexNeighbors.FindOrAdd(I2).AddUnique(I0);
				VertexNeighbors.FindOrAdd(I2).AddUnique(I1);
			}
		}

		// Laplacian smoothing iterations
		for (int32 Iter = 0; Iter < Config.SmoothingIterations; Iter++)
		{
			TArray<FVector> SmoothedVertices = NewVertices;
			
			for (auto& Pair : VertexNeighbors)
			{
				int32 VertIdx = Pair.Key;
				const TArray<int32>& Neighbors = Pair.Value;
				
				if (Neighbors.Num() == 0) continue;
				
				// Calculate average Z of neighbors
				double AvgNeighborZ = 0.0;
				for (int32 N : Neighbors)
				{
					AvgNeighborZ += NewVertices[N].Z;
				}
				AvgNeighborZ /= Neighbors.Num();
				
				// Blend current Z towards neighbor average (50% blend)
				SmoothedVertices[VertIdx].Z = FMath::Lerp(NewVertices[VertIdx].Z, (float)AvgNeighborZ, 0.5f);
			}
			
			NewVertices = SmoothedVertices;
		}
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

FShrinkWrapResult ACesiumShrinkWrapActor::WrapCesiumPrimitive(UPrimitiveComponent* Component, ICesiumPrimitive* CesiumPrimitive)
{
	FShrinkWrapResult Result;

	if (!Component || !CesiumPrimitive)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] WrapCesiumPrimitive - NULL component or interface"));
		return Result;
	}

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Processing Cesium primitive: %s"), *Component->GetName());

	// Get vertex data from Cesium's CPU-safe PositionAccessor
	const CesiumPrimitiveData& PrimData = CesiumPrimitive->getPrimitiveData();
	const auto& PositionAccessor = PrimData.PositionAccessor;

	const int64 NumVertices = PositionAccessor.size();
	if (NumVertices == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Cesium primitive %s has 0 vertices"), *Component->GetName());
		return Result;
	}

	UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Cesium primitive has %lld vertices"), NumVertices);

	Result.TotalVertices = static_cast<int32>(NumVertices);

	// Get component transform
	FTransform MeshTransform = Component->GetComponentToWorld();
	if (!MeshTransform.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[ShrinkWrap] Invalid transform for Cesium primitive %s"), *Component->GetName());
		return Result;
	}

	// Cesium's scale factor for vertex positions
	const FVector ScaleFactor = FVector(
		CesiumPrimitiveData::positionScaleFactor,
		-CesiumPrimitiveData::positionScaleFactor,  // Y-flip for glTF→Unreal handedness
		CesiumPrimitiveData::positionScaleFactor
	);

	FVector RayDirection = GetRaycastDirection();

	// Extract and raycast vertices
	TArray<FVector> LocalVertices;
	TArray<FVector> NewVertices;
	LocalVertices.SetNum(static_cast<int32>(NumVertices));
	NewVertices.SetNum(static_cast<int32>(NumVertices));

	TArray<bool> VertexHit;
	VertexHit.SetNum(static_cast<int32>(NumVertices));
	double TotalWrappedZ = 0.0;
	int32 WrappedCount = 0;

	// First pass: raycast all vertices
	for (int64 i = 0; i < NumVertices; i++)
	{
		// Read raw glTF position from CPU-safe accessor
		const FVector3f& RawGltfPos = PositionAccessor[i];

		// Skip invalid positions
		if (!FMath::IsFinite(RawGltfPos.X) ||
			!FMath::IsFinite(RawGltfPos.Y) ||
			!FMath::IsFinite(RawGltfPos.Z))
		{
			LocalVertices[i] = FVector::ZeroVector;
			NewVertices[i] = FVector::ZeroVector;
			VertexHit[i] = false;
			Result.VerticesMissed++;
			continue;
		}

		// Apply Cesium's internal transformation (scale + Y-flip)
		const FVector LocalPos(
			RawGltfPos.X * ScaleFactor.X,
			RawGltfPos.Y * ScaleFactor.Y,
			RawGltfPos.Z * ScaleFactor.Z
		);
		LocalVertices[i] = LocalPos;

		// Transform to world space
		FVector WorldPosition = MeshTransform.TransformPosition(LocalPos);

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
			VertexHit[i] = true;
			TotalWrappedZ += FinalWorldPosition.Z;
			WrappedCount++;
			Result.VerticesWrapped++;
		}
		else
		{
			NewVertices[i] = LocalPos;
			DebugPoint.TargetPosition = WorldPosition;
			DebugPoint.bHit = false;
			VertexHit[i] = false;
			Result.VerticesMissed++;
		}

		DebugPoints.Add(DebugPoint);
	}

	// Skip this mesh if no vertices were wrapped
	if (Config.bSkipFullyMissedMeshes && WrappedCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Skipping Cesium primitive %s - no terrain hits"), *Component->GetName());
		Result.bSuccess = false;
		return Result;
	}

	// Check minimum hit ratio
	float HitRatio = static_cast<float>(WrappedCount) / static_cast<float>(NumVertices);
	if (HitRatio < Config.MinHitRatio)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Skipping Cesium primitive %s - hit ratio %.1f%% below threshold %.1f%%"), 
			*Component->GetName(), HitRatio * 100.0f, Config.MinHitRatio * 100.0f);
		Result.bSuccess = false;
		return Result;
	}

	// Second pass: handle missed vertices with IDW if configured
	if (Config.MissedVertexHandling == 3 && Result.VerticesMissed > 0 && WrappedCount > 0)
	{
		// Pre-compute wrapped vertex world positions for IDW lookup
		TArray<FVector> WrappedWorldPositions;
		WrappedWorldPositions.Reserve(WrappedCount);
		for (int32 i = 0; i < NewVertices.Num(); i++)
		{
			if (VertexHit[i])
			{
				WrappedWorldPositions.Add(MeshTransform.TransformPosition(NewVertices[i]));
			}
		}

		const float SearchRadiusSq = Config.NeighborSearchRadius * Config.NeighborSearchRadius;
		double AverageWrappedZ = TotalWrappedZ / WrappedCount;

		for (int32 i = 0; i < NewVertices.Num(); i++)
		{
			if (!VertexHit[i])
			{
				FVector WorldPosition = MeshTransform.TransformPosition(LocalVertices[i]);
				FVector2D MissedXY(WorldPosition.X, WorldPosition.Y);

				double WeightedSum = 0.0;
				double TotalWeight = 0.0;
				int32 NeighborsUsed = 0;

				for (const FVector& WrappedPos : WrappedWorldPositions)
				{
					FVector2D WrappedXY(WrappedPos.X, WrappedPos.Y);
					float DistSq = FVector2D::DistSquared(MissedXY, WrappedXY);

					if (DistSq < SearchRadiusSq && DistSq > 1.0f)  // Avoid division by zero
					{
						double Weight = 1.0 / FMath::Pow(FMath::Sqrt(DistSq), Config.IDWPower);
						WeightedSum += WrappedPos.Z * Weight;
						TotalWeight += Weight;
						NeighborsUsed++;
					}
				}

				FVector AdjustedPosition = WorldPosition;
				if (TotalWeight > 0.0 && NeighborsUsed >= 1)
				{
					AdjustedPosition.Z = WeightedSum / TotalWeight;
				}
				else
				{
					AdjustedPosition.Z = AverageWrappedZ;
				}
				NewVertices[i] = MeshTransform.InverseTransformPosition(AdjustedPosition);
			}
		}
	}
	else if (Config.MissedVertexHandling == 1 && Result.VerticesMissed > 0 && WrappedCount > 0)
	{
		// Use global average Z
		double AverageWrappedZ = TotalWrappedZ / WrappedCount;
		for (int32 i = 0; i < NewVertices.Num(); i++)
		{
			if (!VertexHit[i])
			{
				FVector WorldPosition = MeshTransform.TransformPosition(LocalVertices[i]);
				WorldPosition.Z = AverageWrappedZ;
				NewVertices[i] = MeshTransform.InverseTransformPosition(WorldPosition);
			}
		}
	}

	// Create procedural mesh replacement
	// For Cesium primitives, we need to extract or generate indices
	TArray<int32> Indices;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;

	// Try to get index data from Cesium primitive if available
	// IndexAccessor is a std::variant, so we need to visit it properly
	const auto& IndexAccessor = PrimData.IndexAccessor;
	bool bHasIndices = false;
	
	std::visit([&](auto&& arg) {
		using T = std::decay_t<decltype(arg)>;
		if constexpr (!std::is_same_v<T, std::monostate>)
		{
			// This is a valid accessor type (uint8, uint16, or uint32)
			int64 IndexCount = arg.size();
			if (IndexCount > 0)
			{
				bHasIndices = true;
				Indices.Reserve(static_cast<int32>(IndexCount));
				for (int64 i = 0; i < IndexCount; i++)
				{
					Indices.Add(static_cast<int32>(arg[i]));
				}
			}
		}
	}, IndexAccessor);
	
	if (!bHasIndices)
	{
		// Generate simple triangle list (every 3 vertices form a triangle)
		Indices.Reserve(static_cast<int32>(NumVertices));
		for (int32 i = 0; i < static_cast<int32>(NumVertices); i++)
		{
			Indices.Add(i);
		}
	}

	// Generate placeholder UVs and tangents; normals will be calculated from triangles
	Normals.SetNum(static_cast<int32>(NumVertices));
	UVs.SetNum(static_cast<int32>(NumVertices));
	Tangents.SetNum(static_cast<int32>(NumVertices));
	
	// Initialize normals to zero for accumulation
	for (int32 i = 0; i < static_cast<int32>(NumVertices); i++)
	{
		Normals[i] = FVector::ZeroVector;
		UVs[i] = FVector2D(0, 0);
		Tangents[i] = FProcMeshTangent(1, 0, 0);
	}
	
	// Calculate face normals and accumulate to vertices
	for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
	{
		int32 i0 = Indices[i];
		int32 i1 = Indices[i + 1];
		int32 i2 = Indices[i + 2];
		
		if (i0 < NewVertices.Num() && i1 < NewVertices.Num() && i2 < NewVertices.Num())
		{
			FVector Edge1 = NewVertices[i1] - NewVertices[i0];
			FVector Edge2 = NewVertices[i2] - NewVertices[i0];
			FVector FaceNormal = FVector::CrossProduct(Edge1, Edge2).GetSafeNormal();
			
			Normals[i0] += FaceNormal;
			Normals[i1] += FaceNormal;
			Normals[i2] += FaceNormal;
		}
	}
	
	// Normalize the accumulated normals
	for (int32 i = 0; i < Normals.Num(); i++)
	{
		if (!Normals[i].IsNearlyZero())
		{
			Normals[i].Normalize();
		}
		else
		{
			Normals[i] = FVector(0, 0, 1);  // Default up for degenerate cases
		}
	}

	// Create procedural mesh
	UProceduralMeshComponent* ProcMesh = NewObject<UProceduralMeshComponent>(this, NAME_None, RF_Transient);
	if (ProcMesh)
	{
		ProcMesh->RegisterComponent();
		ProcMesh->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		ProcMesh->SetWorldTransform(MeshTransform);
		
		// Ensure procedural mesh is visible
		ProcMesh->SetVisibility(true);
		ProcMesh->SetHiddenInGame(false);

		// Copy materials from original component
		UMaterialInterface* OriginalMaterial = nullptr;
		for (int32 i = 0; i < Component->GetNumMaterials(); i++)
		{
			UMaterialInterface* Mat = Component->GetMaterial(i);
			if (Mat)
			{
				ProcMesh->SetMaterial(i, Mat);
				OriginalMaterial = Mat;
				UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Copied material %d: %s"), i, *Mat->GetName());
			}
		}
		
		if (!OriginalMaterial)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] WARNING: No material found on original component!"));
		}

		// Create mesh section with double-sided rendering
		ProcMesh->CreateMeshSection_LinearColor(
			0,
			NewVertices,
			Indices,
			Normals,
			UVs,
			TArray<FLinearColor>(),
			Tangents,
			true  // Create collision
		);
		
		// Enable double-sided geometry to prevent backface culling issues
		ProcMesh->SetCastShadow(true);
		
		// Log bounds for debugging
		FBox Bounds = ProcMesh->Bounds.GetBox();
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] ProcMesh bounds: Min=%s, Max=%s"), 
			*Bounds.Min.ToString(), *Bounds.Max.ToString());

		CreatedProceduralMeshes.Add(ProcMesh);
		
		// Hide original Cesium component and track it for reset
		Component->SetVisibility(false);
		Component->SetHiddenInGame(true);
		HiddenCesiumPrimitives.Add(Component);
		
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Created procedural mesh for Cesium primitive at location: %s"), 
			*MeshTransform.GetLocation().ToString());
		UE_LOG(LogTemp, Warning, TEXT("[ShrinkWrap] Wrapped: %d, Missed: %d, ProcMesh Visible: %d, NumSections: %d"), 
			Result.VerticesWrapped, Result.VerticesMissed, ProcMesh->IsVisible(), ProcMesh->GetNumSections());
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
			MeshComp->SetHiddenInGame(false);
		}
	}
	HiddenOriginalMeshes.Empty();

	// Show hidden Cesium primitives
	for (UPrimitiveComponent* PrimComp : HiddenCesiumPrimitives)
	{
		if (PrimComp)
		{
			PrimComp->SetVisibility(true);
			PrimComp->SetHiddenInGame(false);
		}
	}
	HiddenCesiumPrimitives.Empty();

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
