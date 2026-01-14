// UnityTransformReceiver.cpp
// Implementation of UDP receiver for Unity flight simulator bridge

#include "UnityTransformReceiver.h"
#include "CesiumGeoreference.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"

AUnityTransformReceiver::AUnityTransformReceiver()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void AUnityTransformReceiver::BeginPlay()
{
    Super::BeginPlay();
    StartUDPReceiver();
}

void AUnityTransformReceiver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopUDPReceiver();
    Super::EndPlay(EndPlayReason);
}

void AUnityTransformReceiver::StartUDPReceiver()
{
    // Parse IP address
    FIPv4Address IP;
    FIPv4Address::Parse(ListenIP, IP);
    FIPv4Endpoint Endpoint(IP, ListenPort);

    // Create UDP socket
    Socket = FUdpSocketBuilder(TEXT("UnityBridgeSocket"))
        .AsNonBlocking()
        .AsReusable()
        .BoundToEndpoint(Endpoint)
        .WithReceiveBufferSize(2 * 1024 * 1024)
        .Build();

    if (Socket)
    {
        // Create async receiver
        FTimespan ThreadWaitTime = FTimespan::FromMilliseconds(10);
        UDPReceiver = new FUdpSocketReceiver(Socket, ThreadWaitTime, TEXT("UnityBridgeReceiver"));
        UDPReceiver->OnDataReceived().BindUObject(this, &AUnityTransformReceiver::OnDataReceived);
        UDPReceiver->Start();
        
        UE_LOG(LogTemp, Log, TEXT("[UnityBridge] Started listening on %s:%d"), *ListenIP, ListenPort);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[UnityBridge] Failed to create socket on port %d"), ListenPort);
    }
}

void AUnityTransformReceiver::StopUDPReceiver()
{
    if (UDPReceiver)
    {
        UDPReceiver->Stop();
        delete UDPReceiver;
        UDPReceiver = nullptr;
    }

    if (Socket)
    {
        Socket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        Socket = nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("[UnityBridge] Stopped. Total packets received: %d"), PacketsReceived);
}

void AUnityTransformReceiver::OnDataReceived(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint)
{
    // Convert bytes to string
    TArray<uint8> ReceivedBytes;
    ReceivedBytes.SetNumUninitialized(Data->TotalSize());
    Data->Serialize(ReceivedBytes.GetData(), Data->TotalSize());
    
    // Null-terminate and convert to FString
    ReceivedBytes.Add(0);
    FString DataString = UTF8_TO_TCHAR(reinterpret_cast<const char*>(ReceivedBytes.GetData()));
    
    // Parse and queue for game thread
    FUnityAircraftData ParsedData;
    if (ParsePacket(DataString, ParsedData))
    {
        DataQueue.Enqueue(ParsedData);
        PacketsReceived++;
        
        if (bDebugLog)
        {
            UE_LOG(LogTemp, Log, TEXT("[UnityBridge] Received: %s (ECEF: X=%f, Y=%f, Z=%f)"), 
                *ParsedData.AircraftId, ParsedData.EcefX, ParsedData.EcefY, ParsedData.EcefZ);
        }
    }
}

bool AUnityTransformReceiver::ParsePacket(const FString& DataString, FUnityAircraftData& OutData)
{
    // Parse CSV: timestamp,id,ecefX,ecefY,ecefZ,quatX,quatY,quatZ,quatW
    TArray<FString> Parts;
    DataString.ParseIntoArray(Parts, TEXT(","), false);
    
    // Minimum 9 fields required (timestamp, id, ecefXYZ, quatXYZW)
    if (Parts.Num() < 9)
    {
        if (bDebugLog)
        {
            UE_LOG(LogTemp, Warning, TEXT("[UnityBridge] Invalid packet (need 9+ fields): %s"), *DataString);
        }
        return false;
    }
    
    // Parse required fields
    OutData.Timestamp = FCString::Atoi64(*Parts[0]);
    OutData.AircraftId = Parts[1].TrimStartAndEnd();
    
    // ECEF Position (indices 2,3,4) - in meters
    OutData.EcefX = FCString::Atod(*Parts[2]);
    OutData.EcefY = FCString::Atod(*Parts[3]);
    OutData.EcefZ = FCString::Atod(*Parts[4]);
    
    // Raw Quaternion (indices 5,6,7,8) - store as-is, no conversion
    OutData.QuatX = FCString::Atof(*Parts[5]);
    OutData.QuatY = FCString::Atof(*Parts[6]);
    OutData.QuatZ = FCString::Atof(*Parts[7]);
    OutData.QuatW = FCString::Atof(*Parts[8]);
    
    if (bDebugLog)
    {
        UE_LOG(LogTemp, Log, TEXT("[UnityBridge] ECEF Pos: (%f, %f, %f), Quat: (%f, %f, %f, %f)"), 
            OutData.EcefX, OutData.EcefY, OutData.EcefZ,
            OutData.QuatX, OutData.QuatY, OutData.QuatZ, OutData.QuatW);
    }
    
    return true;
}

void AUnityTransformReceiver::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    
    // Try to find target aircraft if not assigned
    if (!TargetAircraft && bAutoFindTarget)
    {
        float CurrentTime = GetWorld()->GetTimeSeconds();
        if (CurrentTime - LastTargetSearchTime >= TargetSearchInterval)
        {
            TryFindTargetAircraft();
            LastTargetSearchTime = CurrentTime;
        }
    }
    
    // Dequeue all pending data, keeping only the latest
    FUnityAircraftData TempData;
    while (DataQueue.Dequeue(TempData))
    {
        LatestData = TempData;
        LastPacketTime = GetWorld()->GetTimeSeconds();
    }
    
    // Apply transform to target actor
    ApplyTransformToTarget();
}

void AUnityTransformReceiver::ApplyTransformToTarget()
{
    // Early out if no target assigned
    if (!TargetAircraft)
    {
        return;
    }
    
    // Early out if no data received yet
    if (LatestData.Timestamp == 0)
    {
        return;
    }
    
    // Log first successful position update
    if (!bFirstPositionApplied)
    {
        UE_LOG(LogTemp, Log, TEXT("[UnityBridge] First position received! ECEF: (%f, %f, %f)"),
            LatestData.EcefX, LatestData.EcefY, LatestData.EcefZ);
        bFirstPositionApplied = true;
    }
    
    // Get Cesium Georeference from the world
    ACesiumGeoreference* GeoRef = ACesiumGeoreference::GetDefaultGeoreference(GetWorld());
    if (!GeoRef)
    {
        if (bDebugLog)
        {
            UE_LOG(LogTemp, Warning, TEXT("[UnityBridge] No CesiumGeoreference found in world!"));
        }
        return;
    }
    
    // Convert ECEF position to Unreal world position
    FVector EcefPosition(LatestData.EcefX, LatestData.EcefY, LatestData.EcefZ);
    FVector UnrealPosition = GeoRef->TransformEarthCenteredEarthFixedPositionToUnreal(EcefPosition);
    
    // Apply position to target actor
    TargetAircraft->SetActorLocation(UnrealPosition);
    
    // ============================================================
    // Unity to Unreal Quaternion Conversion (Direct, no ECEF)
    // Unity: Left-handed, Y-up (X=Right, Y=Up, Z=Forward)
    // Unreal: Left-handed, Z-up (X=Forward, Y=Right, Z=Up)
    // ============================================================
    
    // Direct coordinate system conversion:
    // Unity X (Right)   -> Unreal Y (Right)
    // Unity Y (Up)      -> Unreal Z (Up)
    // Unity Z (Forward) -> Unreal X (Forward)
    // Corrected quaternion mapping based on observed behavior
    FQuat FinalQuat(
        LatestData.QuatX,    // Unreal X = Unity X
        -LatestData.QuatZ,    // Unreal Y = Unity Z
        LatestData.QuatY,    // Unreal Z = Unity Y (up)
        LatestData.QuatW     // W stays the same
    );
    FinalQuat.Normalize();
    
    // Apply 180-degree yaw correction (rotate around Z axis) to fix backwards facing
    FQuat YawCorrection = FQuat(FVector::UpVector, PI);  // 180 degrees around Z
    FinalQuat = FinalQuat * YawCorrection;
    FinalQuat.Normalize();
    
    // Apply rotation to target actor
    TargetAircraft->SetActorRotation(FinalQuat);
    
    if (bDebugLog)
    {
        FRotator Rot = FinalQuat.Rotator();
        UE_LOG(LogTemp, Log, TEXT("[UnityBridge] Pos: (%f, %f, %f), Rot: P=%f, Y=%f, R=%f"),
            UnrealPosition.X, UnrealPosition.Y, UnrealPosition.Z,
            Rot.Pitch, Rot.Yaw, Rot.Roll);
    }
    
    // Debug visualization
    if (bDrawDebugPosition)
    {
        DrawDebugSphere(GetWorld(), UnrealPosition, 100.0f, 8, FColor::White, false, -1.0f, 0, 2.0f);
        
        // Red = Forward (X)
        FVector ForwardDir = TargetAircraft->GetActorForwardVector();
        DrawDebugLine(GetWorld(), UnrealPosition, UnrealPosition + ForwardDir * 500.0f, FColor::Red, false, -1.0f, 0, 3.0f);
        
        // Green = Right (Y)
        FVector RightDir = TargetAircraft->GetActorRightVector();
        DrawDebugLine(GetWorld(), UnrealPosition, UnrealPosition + RightDir * 300.0f, FColor::Green, false, -1.0f, 0, 3.0f);
        
        // Blue = Up (Z)
        FVector UpDir = TargetAircraft->GetActorUpVector();
        DrawDebugLine(GetWorld(), UnrealPosition, UnrealPosition + UpDir * 300.0f, FColor::Blue, false, -1.0f, 0, 3.0f);
    }
}

void AUnityTransformReceiver::TryFindTargetAircraft()
{
    if (TargetActorNamePattern.IsEmpty())
    {
        return;
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor != this)
        {
            FString ActorName = Actor->GetName();
            
            if (ActorName.Contains(TargetActorNamePattern, ESearchCase::IgnoreCase))
            {
                TargetAircraft = Actor;
                UE_LOG(LogTemp, Log, TEXT("[UnityBridge] Found target aircraft: %s"), *ActorName);
                return;
            }
        }
    }
    
    if (bDebugLog)
    {
        UE_LOG(LogTemp, Warning, TEXT("[UnityBridge] Could not find actor matching pattern: %s"), *TargetActorNamePattern);
    }
}
