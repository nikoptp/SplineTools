#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralRoadActor.h"
#include "RoadNetworkActor.generated.h"

class AProceduralRoadJunctionActor;
class ALandscape;
class ARoadNetworkLandscapeBrush;
class ULandscapeLayerInfoObject;

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadNetworkPoint
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid Id;

	UPROPERTY()
	FVector WorldLocation = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	FVector Scale = FVector::OneVector;

	UPROPERTY()
	TEnumAsByte<ESplinePointType::Type> Type = ESplinePointType::Curve;
};

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadNetworkLink
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid Id;

	UPROPERTY()
	FGuid StartPointId;

	UPROPERTY()
	FGuid EndPointId;

	UPROPERTY(EditAnywhere, Category = "Road")
	TSoftClassPtr<AProceduralRoadActor> RoadClass;

	UPROPERTY()
	bool bHasCustomTangents = false;

	UPROPERTY()
	FVector StartLeaveTangent = FVector::ZeroVector;

	UPROPERTY()
	FVector EndArriveTangent = FVector::ZeroVector;
};

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadGeneratedRun
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid Id;

	UPROPERTY()
	TArray<FGuid> LinkIds;

	UPROPERTY()
	TSoftObjectPtr<AProceduralRoadActor> RoadActor;
};

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadGeneratedJunction
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid NodeId;

	UPROPERTY()
	TSoftObjectPtr<AProceduralRoadJunctionActor> JunctionActor;
};

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadGeneratedLandscapeBrush
{
	GENERATED_BODY()

	UPROPERTY()
	TSoftObjectPtr<ALandscape> Landscape;

	UPROPERTY()
	FName EditLayerName;

	UPROPERTY()
	TSoftObjectPtr<ARoadNetworkLandscapeBrush> BrushActor;
};

UCLASS(NotBlueprintable)
class SPLINETOOLSEDITOR_API ARoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ARoadNetworkActor();

	virtual bool IsEditorOnly() const override;
	virtual void Destroyed() override;

#if WITH_EDITOR
	virtual void PostEditUndo() override;
#endif

	bool AddPaintedStroke(
		const TArray<FVector>& SampledPoints,
		TSubclassOf<AProceduralRoadActor> RoadClass);
	bool InsertPointOnLink(const FGuid& LinkId);
	bool MovePointToLandscape(const FGuid& PointId, const FVector& WorldLocation);
	bool DeletePoint(const FGuid& PointId);
	bool DeleteLink(const FGuid& LinkId);
	bool DeleteSelection();
	bool AdoptSelectedRoads();
	bool ProjectToLandscape(const FVector& DesiredLocation, FVector& OutLocation) const;
	void BuildStrokePreview(
		const TArray<FVector>& SampledPoints,
		TArray<FVector>& OutSimplifiedPoints,
		TArray<FVector>& OutIntersectionPoints) const;
	bool FindSnapPreviewTarget(
		const FVector& WorldLocation,
		FVector& OutWorldLocation) const;
	void GetLinkWorldSamples(
		const FRoadNetworkLink& Link,
		TArray<FVector>& OutWorldPoints) const;

	void SetSelection(const FGuid& PointId, const FGuid& LinkId);
	FGuid GetSelectedPointId() const;
	FGuid GetSelectedLinkId() const;
	const TArray<FRoadNetworkPoint>& GetPoints() const;
	const TArray<FRoadNetworkLink>& GetLinks() const;
	int32 GetGeneratedJunctionCount() const;
	bool HasPendingRebuild() const;
	bool ValidateNetworkGraph(FString& OutErrors) const;
	FText GetLandscapePaintStatusText() const;
	const FRoadNetworkPoint* FindPoint(const FGuid& PointId) const;
	const FRoadNetworkLink* FindLink(const FGuid& LinkId) const;

	UFUNCTION(CallInEditor, Category = "Road Painting")
	void RebuildDirty();

	UFUNCTION(CallInEditor, Category = "Road Painting")
	void RebuildAll();

	UFUNCTION(CallInEditor, Category = "Road Painting")
	void AdoptSelected();

	UFUNCTION(CallInEditor, Category = "Road Painting")
	void ValidateNetwork();

	UPROPERTY(EditAnywhere, Category = "Road Painting|Classes")
	TSoftClassPtr<AProceduralRoadActor> DefaultRoadClass;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Classes")
	TSoftClassPtr<AProceduralRoadJunctionActor> JunctionClass;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Brush", meta = (ClampMin = "10.0"))
	float StrokeSampleSpacing = 200.0f;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Brush", meta = (ClampMin = "0.0"))
	float SimplificationTolerance = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Connections", meta = (ClampMin = "1.0"))
	float SnapRadius = 300.0f;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Connections", meta = (ClampMin = "10.0"))
	float CrossingSampleInterval = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Connections", meta = (ClampMin = "1.0"))
	float IntersectionMergeRadius = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Connections", meta = (ClampMin = "0.0"))
	float MaximumJunctionHeightDifference = 200.0f;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Connections", meta = (ClampMin = "0.0"))
	float JunctionTrimDistance = 300.0f;

private:
	struct FGeneratedRunCandidate;
	struct FStrokeIntersection;

	void SimplifyStroke(
		const TArray<FVector>& InputPoints,
		TArray<FVector>& OutPoints) const;
	void SimplifyRange(
		const TArray<FVector>& InputPoints,
		int32 FirstIndex,
		int32 LastIndex,
		TArray<bool>& KeepPoints) const;
	FGuid AddPoint(const FVector& WorldLocation);
	FGuid FindOrAddPoint(const FVector& WorldLocation);
	FGuid ResolveStrokeEndpoint(const FVector& WorldLocation);
	bool SplitLinkAtLocation(
		const FGuid& LinkId,
		const FVector& WorldLocation,
		FGuid& OutPointId,
		float SplitAlpha = -1.0f);
	bool AddLink(
		const FGuid& StartPointId,
		const FGuid& EndPointId,
		TSubclassOf<AProceduralRoadActor> RoadClass,
		const FRoadNetworkLink* SourceLink = nullptr);
	void SampleLinkPath(
		const FRoadNetworkLink& Link,
		TArray<FVector>& OutWorldPoints) const;
	void RemoveIsolatedPoints();
	void MarkPointConnectionsDirty(const FGuid& PointId);
	void BuildAdjacency(TMap<FGuid, TArray<int32>>& OutAdjacency) const;
	void BuildRunCandidates(TArray<FGeneratedRunCandidate>& OutRuns) const;
	void RebuildGeneratedActors();
	bool RebuildLandscapeMaterialPaint();
	void FindLoadedManagedActors(
		TArray<AProceduralRoadActor*>& OutRoadActors,
		TArray<AProceduralRoadJunctionActor*>& OutJunctionActors) const;
	void FindLoadedManagedLandscapeBrushes(
		TArray<ARoadNetworkLandscapeBrush*>& OutBrushes) const;
	AProceduralRoadActor* FindReusableRoad(
		const FGeneratedRunCandidate& Run,
		TSet<FGuid>& UsedRunIds,
		FGuid& OutRunId) const;
	AProceduralRoadJunctionActor* FindReusableJunction(const FGuid& NodeId) const;
	bool ValidateGraph(FString& OutErrors) const;

#if WITH_EDITOR
	void QueueUndoRebuild();
	void CancelQueuedUndoRebuild();
#endif

	UPROPERTY()
	FGuid NetworkId;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Graph")
	TArray<FRoadNetworkPoint> Points;

	UPROPERTY(EditAnywhere, Category = "Road Painting|Graph")
	TArray<FRoadNetworkLink> Links;

	UPROPERTY()
	TArray<FRoadGeneratedRun> GeneratedRuns;

	UPROPERTY()
	TArray<FRoadGeneratedJunction> GeneratedJunctions;

	UPROPERTY()
	TArray<FRoadGeneratedLandscapeBrush> GeneratedLandscapeBrushes;

	/** Hidden migration data from the original network-owned Landscape paint settings. */
	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Configure Landscape paint in the road Blueprint Class Defaults."))
	bool bPaintLandscapeMaterial = false;

	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Configure Landscape paint in the road Blueprint Class Defaults."))
	TSoftObjectPtr<ULandscapeLayerInfoObject> LandscapePaintLayer;

	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Configure Landscape paint in the road Blueprint Class Defaults."))
	FName LandscapePaintEditLayer = TEXT("RoadPainting");

	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Configure Landscape paint in the road Blueprint Class Defaults."))
	float LandscapePaintWidth = 500.0f;

	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Configure Landscape paint in the road Blueprint Class Defaults."))
	float LandscapePaintFalloff = 300.0f;

	UPROPERTY(Transient)
	FGuid SelectedPointId;

	UPROPERTY(Transient)
	FGuid SelectedLinkId;

	UPROPERTY(Transient)
	TSet<FGuid> DirtyLinkIds;

	UPROPERTY(Transient)
	TSet<FGuid> DirtyPointIds;

	UPROPERTY(Transient)
	bool bForceFullRebuild = true;

	UPROPERTY(Transient)
	bool bForceJunctionRebuild = false;

	FString LandscapePaintStatus;

#if WITH_EDITOR
	FTSTicker::FDelegateHandle UndoRebuildTickerHandle;
#endif
};
