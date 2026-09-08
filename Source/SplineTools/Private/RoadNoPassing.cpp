#include "RoadNoPassing.h"

namespace
{
	FVector SamplePosition(const TArray<FRoadNoPassingSample>& Samples, float Distance)
	{
		int32 Low = 0;
		int32 High = Samples.Num() - 1;
		while (Low + 1 < High)
		{
			const int32 Middle = (Low + High) / 2;
			if (Samples[Middle].Distance <= Distance)
			{
				Low = Middle;
			}
			else
			{
				High = Middle;
			}
		}
		return FMath::Lerp(Samples[Low].Position, Samples[High].Position, FMath::Clamp((Distance - Samples[Low].Distance) / (Samples[High].Distance - Samples[Low].Distance), 0.0f, 1.0f));
	}

	void AppendMerged(TArray<FRoadNoPassingRange>& Ranges, float Start, float End, int32 SideSign)
	{
		if (End <= Start)
		{
			return;
		}
		if (!Ranges.IsEmpty() && Start <= Ranges.Last().End + KINDA_SMALL_NUMBER)
		{
			Ranges.Last().End = FMath::Max(Ranges.Last().End, End);
		}
		else
		{
			Ranges.Add({Start, End, SideSign});
		}
	}
}

TArray<FRoadNoPassingRange> RoadNoPassing::Analyze(const TArray<FRoadNoPassingSample>& Samples, const FRoadNoPassingSettings& Settings)
{
	TArray<FRoadNoPassingRange> Result;
	if (Samples.Num() < 3)
	{
		return Result;
	}

	TArray<FRoadNoPassingRange> Left;
	TArray<FRoadNoPassingRange> Right;
	for (int32 Index = 1; Index + 1 < Samples.Num(); ++Index)
	{
		const FVector Incoming = Samples[Index].Position - SamplePosition(Samples, Samples[Index].Distance - FMath::Max(Settings.AnalysisDistance, 1.0f));
		const FVector Outgoing = SamplePosition(Samples, Samples[Index].Distance + FMath::Max(Settings.AnalysisDistance, 1.0f)) - Samples[Index].Position;
		if (Incoming.SizeSquared2D() <= KINDA_SMALL_NUMBER || Outgoing.SizeSquared2D() <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const double Turn = FMath::RadiansToDegrees(FMath::Atan2(Incoming.X * Outgoing.Y - Incoming.Y * Outgoing.X, Incoming.X * Outgoing.X + Incoming.Y * Outgoing.Y));
		const double PitchDecrease = FMath::RadiansToDegrees(FMath::Atan2(Incoming.Z, Incoming.Size2D()) - FMath::Atan2(Outgoing.Z, Outgoing.Size2D()));
		const bool bCrest = PitchDecrease >= FMath::Max(Settings.CrestThresholdDegrees, 0.01f);
		const float Start = (Samples[Index - 1].Distance + Samples[Index].Distance) * 0.5f;
		const float End = (Samples[Index].Distance + Samples[Index + 1].Distance) * 0.5f;
		if (bCrest || Turn <= -FMath::Max(Settings.BendThresholdDegrees, 0.01f))
		{
			AppendMerged(Left, Start, End, -1);
		}
		if (bCrest || Turn >= FMath::Max(Settings.BendThresholdDegrees, 0.01f))
		{
			AppendMerged(Right, Start, End, 1);
		}
	}

	for (TArray<FRoadNoPassingRange>* Lane : {&Left, &Right})
	{
		TArray<FRoadNoPassingRange> Extended;
		for (const FRoadNoPassingRange& Range : *Lane)
		{
			const float Padding = FMath::Max(0.0f, Settings.MinimumLength - (Range.End - Range.Start)) * 0.5f;
			const float Start = Range.Start - Padding - (Range.SideSign > 0 ? FMath::Max(Settings.AdvanceDistance, 0.0f) : 0.0f);
			const float End = Range.End + Padding + (Range.SideSign < 0 ? FMath::Max(Settings.AdvanceDistance, 0.0f) : 0.0f);
			Extended.Add({FMath::Max(Start, Samples[0].Distance), FMath::Min(End, Samples.Last().Distance), Range.SideSign});
		}
		Extended.Sort([](const FRoadNoPassingRange& A, const FRoadNoPassingRange& B) { return A.Start < B.Start; });
		TArray<FRoadNoPassingRange> Merged;
		for (const FRoadNoPassingRange& Range : Extended)
		{
			AppendMerged(Merged, Range.Start, Range.End, Range.SideSign);
		}
		Result.Append(Merged);
	}
	return Result;
}
