// UnityTransformReceiver.h
// Receives UDP transform data from Unity flight simulator and positions aircraft in Unreal

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "UnityTransformReceiver.generated.h"

/**
 * Struct to hold parsed aircraft data from Unity (ECEF format)
 */
USTRUCT(BlueprintType)
struct FUnityAircraftData
{
    GENERATED_BODY()

    /** Unix timestamp in milliseconds (UTC) */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    int64 Timestamp = 0;

    /** Aircraft identifier string */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    FString AircraftId;

    /** ECEF X coordinate in meters */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    double EcefX = 0.0;

    /** ECEF Y coordinate in meters */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    double EcefY = 0.0;

    /** ECEF Z coordinate in meters */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    double EcefZ = 0.0;

    /** Raw quaternion X from Unity (EUN frame) */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float QuatX = 0.0f;

    /** Raw quaternion Y from Unity (EUN frame) */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float QuatY = 0.0f;

    /** Raw quaternion Z from Unity (EUN frame) */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float QuatZ = 0.0f;

    /** Raw quaternion W from Unity (EUN frame) */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float QuatW = 0.0f;

    /** Camera local quaternion X from Unity */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float CamQuatX = 0.0f;

    /** Camera local quaternion Y from Unity */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float CamQuatY = 0.0f;

    /** Camera local quaternion Z from Unity */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float CamQuatZ = 0.0f;

    /** Camera local quaternion W from Unity */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    float CamQuatW = 1.0f;

    /** Whether camera data was received in this packet */
    UPROPERTY(BlueprintReadOnly, Category = "Aircraft Data")
    bool bHasCameraData = false;
};

/**
 * Actor that receives UDP transform data from Unity and positions a target aircraft
 * using ECEF coordinates for proper Cesium geo-referencing.
 * 
 * Unity sends CSV-formatted strings at 60 Hz with ECEF position and rotation:
 * timestamp,aircraftId,ecefX,ecefY,ecefZ,ecefQuatX,ecefQuatY,ecefQuatZ,ecefQuatW,camQuatX,camQuatY,camQuatZ,camQuatW
 * 
 * ECEF (Earth-Centered, Earth-Fixed) is the native coordinate system for both
 * Cesium for Unity and Cesium for Unreal, ensuring robust synchronization.
 */
UCLASS()
class CESIUMSTART_API AUnityTransformReceiver : public AActor
{
    GENERATED_BODY()

public:
    AUnityTransformReceiver();

    // ==================== UDP Settings ====================
    
    /** Port to listen for UDP packets from Unity */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UDP Settings")
    int32 ListenPort = 9999;

    /** IP address to bind to (0.0.0.0 = all interfaces) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UDP Settings")
    FString ListenIP = TEXT("0.0.0.0");

    // ==================== Target ====================
    
    /** The aircraft actor that will be moved based on received data */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
    AActor* TargetAircraft;

    /** The camera component to sync rotation (will auto-find from TargetAircraft if not set) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
    class UCameraComponent* TargetCamera;

    /** If true, automatically find target aircraft by name if not assigned */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
    bool bAutoFindTarget = true;

    /** Name pattern to search for (e.g., "F16" will match any actor containing "F16" in its name) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
    FString TargetActorNamePattern = TEXT("F16");

    /** How often to search for target if not found (seconds) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
    float TargetSearchInterval = 1.0f;

    // ==================== Interpolation ====================
    
    /** Enable smooth position interpolation (lerp) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interpolation")
    bool bEnablePositionLerp = true;

    /** Speed of position interpolation (higher = faster, snappier movement) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interpolation", meta = (ClampMin = "0.1", ClampMax = "50.0", EditCondition = "bEnablePositionLerp"))
    float PositionLerpSpeed = 15.0f;

    /** Enable smooth rotation interpolation (slerp) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interpolation")
    bool bEnableRotationSlerp = true;

    /** Speed of rotation interpolation (higher = faster, snappier rotation) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interpolation", meta = (ClampMin = "0.1", ClampMax = "50.0", EditCondition = "bEnableRotationSlerp"))
    float RotationSlerpSpeed = 15.0f;

    // ==================== Data ==
    
    /** Latest received aircraft data (for debugging/blueprints) */
    UPROPERTY(BlueprintReadOnly, Category = "Data")
    FUnityAircraftData LatestData;

    // ==================== Statistics ====================
    
    /** Total number of packets successfully received and parsed */
    UPROPERTY(BlueprintReadOnly, Category = "Stats")
    int32 PacketsReceived = 0;

    /** World time when last packet was received */
    UPROPERTY(BlueprintReadOnly, Category = "Stats")
    float LastPacketTime = 0.0f;

    // ==================== Debug ====================
    
    /** Enable verbose logging of received packets */
    UPROPERTY(EditAnywhere, Category = "Debug")
    bool bDebugLog = false;

    /** Draw debug sphere and direction at aircraft position */
    UPROPERTY(EditAnywhere, Category = "Debug")
    bool bDrawDebugPosition = false;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaTime) override;

private:
    /** UDP socket for receiving data */
    FSocket* Socket = nullptr;
    
    /** Async UDP receiver running on background thread */
    FUdpSocketReceiver* UDPReceiver = nullptr;
    
    /** Thread-safe queue for passing data from network thread to game thread */
    TQueue<FUnityAircraftData, EQueueMode::Mpsc> DataQueue;
    
    /** Track if we've logged first position (avoid static variable memory leak) */
    bool bFirstPositionApplied = false;

    /** Current interpolated position for smooth movement */
    FVector CurrentLerpPosition = FVector::ZeroVector;

    /** Whether we have a valid lerp starting position */
    bool bHasLerpPosition = false;

    /** Current interpolated rotation for smooth rotation */
    FQuat CurrentSlerpRotation = FQuat::Identity;

    /** Whether we have a valid slerp starting rotation */
    bool bHasSlerpRotation = false;

    /** Initialize and start the UDP socket receiver */
    void StartUDPReceiver();
    
    /** Stop and cleanup the UDP socket receiver */
    void StopUDPReceiver();
    
    /** Callback when UDP data is received (runs on network thread) */
    void OnDataReceived(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);
    
    /** Parse CSV packet string into aircraft data struct */
    bool ParsePacket(const FString& DataString, FUnityAircraftData& OutData);
    
    /** Apply the latest transform data to the target aircraft using Cesium coordinates */
    void ApplyTransformToTarget();

    /** Try to find target aircraft by name pattern */
    void TryFindTargetAircraft();

    /** Time of last target search attempt */
    float LastTargetSearchTime = 0.0f;
};
