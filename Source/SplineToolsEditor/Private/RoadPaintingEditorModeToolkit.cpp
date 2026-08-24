#include "RoadPaintingEditorModeToolkit.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "RoadPaintingEditorMode.h"
#include "RoadPaintingEditorModeCommands.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RoadPaintingEditorModeToolkit"

void FRoadPaintingEditorModeToolkit::Init(
	const TSharedPtr<IToolkitHost>& InitToolkitHost,
	TWeakObjectPtr<UEdMode> InOwningMode)
{
	Mode = Cast<URoadPaintingEditorMode>(InOwningMode.Get());
	FModeToolkit::Init(InitToolkitHost, InOwningMode);
}

void FRoadPaintingEditorModeToolkit::GetToolPaletteNames(
	TArray<FName>& PaletteNames) const
{
	PaletteNames.Add(NAME_Default);
}

void FRoadPaintingEditorModeToolkit::BuildToolPalette(
	FName Palette,
	FToolBarBuilder& ToolbarBuilder)
{
	if (Palette != NAME_Default)
	{
		return;
	}

	ToolbarBuilder.AddToolBarButton(FRoadPaintingEditorModeCommands::Get().DrawRoad);
	ToolbarBuilder.AddToolBarButton(FRoadPaintingEditorModeCommands::Get().SelectRoad);
}

TSharedPtr<SWidget> FRoadPaintingEditorModeToolkit::GetInlineContent() const
{
	if (InlineContent.IsValid())
	{
		return InlineContent;
	}

	TSharedPtr<SWidget> ToolContent = FModeToolkit::GetInlineContent();
	InlineContent = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SBorder)
			.Padding(6.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([WeakMode = Mode]()
					{
						return WeakMode.IsValid()
							? WeakMode->GetNetworkNameText()
							: LOCTEXT("NoMode", "Road Painting");
					})
					.Font(FCoreStyle::Get().GetFontStyle("BoldFont"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([WeakMode = Mode]()
					{
						return WeakMode.IsValid()
							? WeakMode->GetNetworkSummaryText()
							: FText::GetEmpty();
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([WeakMode = Mode]()
					{
						return WeakMode.IsValid()
							? WeakMode->GetNetworkStatusText()
							: FText::GetEmpty();
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FLinearColor(0.65f, 0.65f, 0.65f)))
					.Text_Lambda([WeakMode = Mode]()
					{
						return WeakMode.IsValid()
							? WeakMode->GetLastOperationText()
							: FText::GetEmpty();
					})
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f)
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 4.0f, 4.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NetworkActions", "Network Actions"))
			.Font(FCoreStyle::Get().GetFontStyle("BoldFont"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 3.0f)
		[
			SNew(SUniformGridPanel)
			.SlotPadding(FMargin(2.0f))
			+ SUniformGridPanel::Slot(0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("AdoptSelectedButton", "Adopt Selected"))
				.ToolTipText(LOCTEXT("AdoptSelectedTooltip", "Import selected procedural roads and junctions into this network."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanAdoptSelectedRoads();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->AdoptSelectedRoads();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(1, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("DeleteButton", "Delete"))
				.ToolTipText(LOCTEXT("DeleteTooltip", "Delete the selected point or link."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanDeleteSelection();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->DeleteSelection();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(0, 1)
			[
				SNew(SButton)
				.Text(LOCTEXT("RebuildDirtyButton", "Rebuild Dirty"))
				.ToolTipText(LOCTEXT("RebuildDirtyTooltip", "Rebuild affected roads and refresh managed junction caches."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanRebuild();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->RebuildDirty();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(1, 1)
			[
				SNew(SButton)
				.Text(LOCTEXT("RebuildAllButton", "Rebuild All"))
				.ToolTipText(LOCTEXT("RebuildAllTooltip", "Regenerate the complete managed road network."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanRebuild();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->RebuildAll();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(0, 2)
			[
				SNew(SButton)
				.Text(LOCTEXT("RebuildLandscapePaintButton", "Rebuild Paint"))
				.ToolTipText(LOCTEXT("RebuildLandscapePaintTooltip", "Synchronize and rasterize road Landscape material masks. Run this after editing roads; normal Landscape painting reuses the cached masks."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanRebuild();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->RebuildLandscapePaint();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(0, 3)
			[
				SNew(SButton)
				.Text(LOCTEXT("ValidateButton", "Validate"))
				.ToolTipText(LOCTEXT("ValidateTooltip", "Check graph identities, links, generated actors, and classes."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanRebuild();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->ValidateNetwork();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(1, 3)
			[
				SNew(SButton)
				.Text(LOCTEXT("FrameButton", "Frame"))
				.ToolTipText(LOCTEXT("FrameTooltip", "Frame the selected element, or the complete road network."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanFrameNetwork();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->FrameNetwork();
					}
					return FReply::Handled();
				})
			]
			+ SUniformGridPanel::Slot(0, 4)
			[
				SNew(SButton)
				.Text(LOCTEXT("InsertPointButton", "Insert Point"))
				.ToolTipText(LOCTEXT("InsertPointTooltip", "Split the selected road link at its midpoint."))
				.IsEnabled_Lambda([WeakMode = Mode]()
				{
					return WeakMode.IsValid() && WeakMode->CanInsertPoint();
				})
				.OnClicked_Lambda([WeakMode = Mode]()
				{
					if (WeakMode.IsValid())
					{
						WeakMode->InsertPoint();
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f)
		[
			SNew(SBorder)
			.Padding(2.0f)
			[
				ToolContent.ToSharedRef()
			]
		];

	return InlineContent;
}

FName FRoadPaintingEditorModeToolkit::GetToolkitFName() const
{
	return FName(TEXT("RoadPaintingEditorMode"));
}

FText FRoadPaintingEditorModeToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("DisplayName", "Road Painting");
}

#undef LOCTEXT_NAMESPACE
