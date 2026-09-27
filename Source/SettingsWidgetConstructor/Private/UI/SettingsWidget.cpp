// Copyright (c) Yevhenii Selivanov

#include "UI/SettingsWidget.h"

// SWC
#include "Data/SettingsDataAsset.h"
#include "MyUtilsLibraries/SWCWidgetUtilsLibrary.h"
#include "MyUtilsLibraries/SettingsUtilsLibrary.h"
#include "UI/SettingCombobox.h"
#include "UI/SettingSubWidget.h"

// UE
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/SizeBox.h"
#include "Components/HorizontalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Viewport.h"
#include "Blueprint/WidgetTree.h"
#include "DataRegistry.h"
#include "DataRegistryTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture.h"
#include "GameFramework/GameUserSettings.h"
#include "GameplayTagsManager.h"
#include "InputCoreTypes.h"
#include "../../public/ui/settingswidget.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/TextBlock.h"
#include "Components/OverlaySlot.h"

#if WITH_EDITOR
#include "Editor.h"
#endif
#include UE_INLINE_GENERATED_CPP_BY_NAME(SettingsWidget)

/* ---------------------------------------------------
 *		Public functions
 * --------------------------------------------------- */

// Try to find the setting row
const FSettingsPicker& USettingsWidget::FindSettingRow(FName PotentialTagName) const
{
	if (PotentialTagName.IsNone())
	{
		return FSettingsPicker::Empty;
	}

	if (const FSettingsPicker* ExactRow = SettingsTableRowsInternal.Find(PotentialTagName))
	{
		return *ExactRow;
	}

	const FString TagSubString = PotentialTagName.ToString();
	const FString TagSuffix = FString(TEXT(".")) + TagSubString;
	const FSettingsPicker* FoundRow = nullptr;
	int32 MatchCount = 0;

	// Prefer a unique leaf/suffix match, e.g. "VSync" -> "Settings.Checkbox.VSync".
	for (const TTuple<FName, FSettingsPicker>& RowIt : SettingsTableRowsInternal)
	{
		const FString TagStringIt = RowIt.Key.ToString();
		if (TagStringIt.Equals(TagSubString) || TagStringIt.EndsWith(TagSuffix))
		{
			FoundRow = &RowIt.Value;
			++MatchCount;
		}
	}

	if (MatchCount == 1)
	{
		return *FoundRow;
	}

	// Preserve the legacy convenience only when the substring is unambiguous.
	FoundRow = nullptr;
	MatchCount = 0;
	for (const TTuple<FName, FSettingsPicker>& RowIt : SettingsTableRowsInternal)
	{
		if (RowIt.Key.ToString().Contains(TagSubString))
		{
			FoundRow = &RowIt.Value;
			++MatchCount;
		}
	}

	return MatchCount == 1 ? *FoundRow : FSettingsPicker::Empty;
}

// Returns the found row by specified tag
const FSettingsPicker& USettingsWidget::GetSettingRow(const FSettingTag& SettingTag) const
{
	if (!SettingTag.IsValid())
	{
		return FSettingsPicker::Empty;
	}

	const FSettingsPicker* FoundRow = SettingsTableRowsInternal.Find(SettingTag.GetTagName());
	return FoundRow ? *FoundRow : FSettingsPicker::Empty;
}

// Save all settings into their configs
void USettingsWidget::SaveSettings()
{
	ApplySettings();

	UGameUserSettings* GameUserSettings = USettingsUtilsLibrary::GetGameUserSettings();
	if (GameUserSettings)
	{
		GameUserSettings->ConfirmVideoMode();
		GameUserSettings->SaveSettings();
	}

	TSet<UObject*> SavedOwners;
	for (const TTuple<FName, FSettingsPicker>& RowIt : SettingsTableRowsInternal)
	{
		if (UObject* ContextObject = RowIt.Value.PrimaryData.GetSettingOwner(this))
		{
			if (ContextObject != GameUserSettings
				&& ContextObject != this
				&& !SavedOwners.Contains(ContextObject))
			{
				ContextObject->SaveConfig();
				SavedOwners.Add(ContextObject);
			}
		}
	}

	CaptureCurrentSettingValues();
	SetHasPendingChanges(false);
}

// Apply all current settings on device
void USettingsWidget::ApplySettings()
{
	UGameUserSettings* GameUserSettings = USettingsUtilsLibrary::GetGameUserSettings();
	if (!GameUserSettings)
	{
		return;
	}

	constexpr bool bCheckForCommandLineOverrides = false;
	GameUserSettings->ApplyResolutionSettings(bCheckForCommandLineOverrides);
	GameUserSettings->ApplyNonResolutionSettings();
	GameUserSettings->RequestUIUpdate();
}

// Update settings on UI
void USettingsWidget::UpdateSettingsByTags(const FGameplayTagContainer& SettingsToUpdate, bool bLoadFromConfig /* = false*/)
{
	if (SettingsToUpdate.IsEmpty()
	    || !SettingsToUpdate.IsValidIndex(0))
	{
		return;
	}

	// A data table can outlive a renamed/removed gameplay tag. Never let one
	// stale row prevent every other setting from loading or updating.
	FGameplayTagContainer RegisteredSettingsToUpdate;
	for (const FGameplayTag& RequestedTag : SettingsToUpdate)
	{
		if (!RequestedTag.IsValid())
		{
			continue;
		}

		const FGameplayTag RegisteredTag =
			UGameplayTagsManager::Get().RequestGameplayTag(RequestedTag.GetTagName(), false);
		if (RegisteredTag.IsValid())
		{
			RegisteredSettingsToUpdate.AddTagFast(RegisteredTag);
		}
	}
	if (RegisteredSettingsToUpdate.IsEmpty())
	{
		return;
	}

	const auto IsRequestedSetting = [&RegisteredSettingsToUpdate](const FSettingTag& SettingTag)
	{
		if (!SettingTag.IsValid())
		{
			return false;
		}

		const FGameplayTag RegisteredTag =
			UGameplayTagsManager::Get().RequestGameplayTag(SettingTag.GetTagName(), false);
		return RegisteredTag.IsValid()
			&& RegisteredTag.MatchesAny(RegisteredSettingsToUpdate);
	};

	if (SettingsTableRowsInternal.IsEmpty())
	{
		CacheTable();
	}

	if (bLoadFromConfig)
	{
		TSet<UObject*> LoadedOwners;
		for (const TTuple<FName, FSettingsPicker>& RowIt : SettingsTableRowsInternal)
		{
			const FSettingsPicker& Setting = RowIt.Value;
			if (!IsRequestedSetting(Setting.PrimaryData.Tag))
			{
				continue;
			}

			UObject* Owner = Setting.PrimaryData.GetSettingOwner(this);
			if (!Owner || LoadedOwners.Contains(Owner))
			{
				continue;
			}

			if (UGameUserSettings* GameUserSettings = Cast<UGameUserSettings>(Owner))
			{
				GameUserSettings->LoadSettings(false);
			}
			else
			{
				Owner->LoadConfig();
			}
			LoadedOwners.Add(Owner);
		}
	}

	TGuardValue<bool> SynchronizingGuard(bIsSynchronizingSettingsInternal, true);
	for (const FName& OrderedTag : OrderedSettingTagsInternal)
	{
		FSettingsPicker* SettingPtr = SettingsTableRowsInternal.Find(OrderedTag);
		if (!SettingPtr)
		{
			continue;
		}

		FSettingsPicker& Setting = *SettingPtr;
		const FSettingTag& SettingTag = Setting.PrimaryData.Tag;
		if (!IsRequestedSetting(SettingTag))
		{
			continue;
		}

		FSettingsDataBase* ChosenData = Setting.GetChosenSettingsData();
		if (!ChosenData
		    || !ChosenData->CanUpdateSetting())
		{
			continue;
		}

		UObject* Owner = Setting.PrimaryData.GetSettingOwner(this);
		if (!Owner)
		{
			continue;
		}

		if (Setting.SettingsType == GET_MEMBER_NAME_CHECKED(FSettingsPicker, Combobox))
		{
			TArray<FText> Members = Setting.Combobox.Members;
			Setting.Combobox.OnGetMembers.ExecuteIfBound(Members);
			Setting.Combobox.Members = Members;
			if (USettingCombobox* ComboboxWidget = GetSettingSubWidget<USettingCombobox>(SettingTag))
			{
				ComboboxWidget->SetComboboxMembers(Members);
			}
		}

		FString Result;
		ChosenData->GetSettingValue(*this, SettingTag, /*Out*/ Result);
		ChosenData->SetSettingValue(*this, SettingTag, Result);
	}
}

// Update all existing settings on UI
void USettingsWidget::UpdateAllSettings(bool bLoadFromConfig)
{
	FGameplayTagContainer AllSettingTags;
	for (const FName& OrderedTag : OrderedSettingTagsInternal)
	{
		if (const FSettingsPicker* Row = SettingsTableRowsInternal.Find(OrderedTag))
		{
			AllSettingTags.AddTagFast(Row->PrimaryData.Tag);
		}
	}
	UpdateSettingsByTags(AllSettingTags, bLoadFromConfig);
}

// Returns the name of found tag by specified function
const FSettingTag& USettingsWidget::GetTagByFunction(const FSettingFunctionPicker& SettingFunction) const
{
	for (const TTuple<FName, FSettingsPicker>& RowIt : SettingsTableRowsInternal)
	{
		const FSettingsPrimary& PrimaryData = RowIt.Value.PrimaryData;
		if (PrimaryData.Getter == SettingFunction
		    || PrimaryData.Setter == SettingFunction)
		{
			return PrimaryData.Tag;
		}
	}

	return FSettingTag::EmptySettingTag;
}

/* ---------------------------------------------------
 *		Setters by setting types
 * --------------------------------------------------- */

// Set value to the option by tag
void USettingsWidget::SetSettingValue(FName TagName, const FString& Value)
{
	const FSettingsPicker& FoundRow = FindSettingRow(TagName);
	if (!FoundRow.IsValid())
	{
		return;
	}

	FSettingsDataBase* ChosenData = FoundRow.GetChosenSettingsData();
	if (!ChosenData)
	{
		return;
	}

	const FSettingTag& Tag = FoundRow.PrimaryData.Tag;
	if (Tag.IsValid())
	{
		ChosenData->SetSettingValue(*this, Tag, Value);
	}
}

// Press button
void USettingsWidget::SetSettingButtonPressed(const FSettingTag& ButtonTag)
{
	if (!ButtonTag.IsValid())
	{
		return;
	}

	const FSettingsPicker* SettingsRowPtr = SettingsTableRowsInternal.Find(ButtonTag.GetTagName());
	if (!SettingsRowPtr)
	{
		return;
	}

	SettingsRowPtr->Button.OnButtonPressed.ExecuteIfBound();

	UpdateSettingsByTags(SettingsRowPtr->PrimaryData.SettingsToUpdate);

	PlayUIClickSFX();
}

// Toggle checkbox
void USettingsWidget::SetSettingCheckbox(const FSettingTag& CheckboxTag, bool InValue)
{
	if (!CheckboxTag.IsValid())
	{
		return;
	}

	FSettingsPicker* FoundRow = SettingsTableRowsInternal.Find(CheckboxTag.GetTagName());
	if (!FoundRow)
	{
		return;
	}

	const bool bChanged = FoundRow->Checkbox.bIsSet != InValue;
	FoundRow->Checkbox.bIsSet = InValue;
	if (bChanged && !bIsSynchronizingSettingsInternal)
	{
		FoundRow->Checkbox.OnSetterBool.ExecuteIfBound(InValue);
	}

	if (USettingCheckbox* SettingCheckbox = GetSettingSubWidget<USettingCheckbox>(CheckboxTag))
	{
		SettingCheckbox->SetCheckboxValue(InValue);
	}

	if (bChanged && !bIsSynchronizingSettingsInternal && !bSuppressChangeNotificationsInternal)
	{
		UpdateSettingsByTags(FoundRow->PrimaryData.SettingsToUpdate);
		OnAnySettingSet(FoundRow->PrimaryData);
		PlayUIClickSFX();
	}
}

// Set chosen member index for a combobox
void USettingsWidget::SetSettingComboboxIndex(const FSettingTag& ComboboxTag, int32 InValue)
{
	if (InValue == INDEX_NONE)
	{
		return;
	}

	if (!ComboboxTag.IsValid())
	{
		return;
	}

	FSettingsPicker* FoundRow = SettingsTableRowsInternal.Find(ComboboxTag.GetTagName());
	if (!FoundRow)
	{
		return;
	}

	if (!FoundRow->Combobox.Members.IsEmpty())
	{
		InValue = FMath::Clamp(InValue, 0, FoundRow->Combobox.Members.Num() - 1);
	}

	const bool bChanged = FoundRow->Combobox.ChosenMemberIndex != InValue;
	FoundRow->Combobox.ChosenMemberIndex = InValue;
	if (bChanged && !bIsSynchronizingSettingsInternal)
	{
		FoundRow->Combobox.OnSetterInt.ExecuteIfBound(InValue);
	}

	if (USettingCombobox* SettingCombobox = GetSettingSubWidget<USettingCombobox>(ComboboxTag))
	{
		SettingCombobox->SetComboboxIndex(InValue);
	}

	if (bChanged && !bIsSynchronizingSettingsInternal && !bSuppressChangeNotificationsInternal)
	{
		UpdateSettingsByTags(FoundRow->PrimaryData.SettingsToUpdate);
		OnAnySettingSet(FoundRow->PrimaryData);
	}
}

// Set current value for a slider
void USettingsWidget::SetSettingSlider(const FSettingTag& SliderTag, double InValue)
{
	static constexpr double MinValue = 0.0;
	static constexpr double MaxValue = 1.0;
	const double NewValue = FMath::Clamp(InValue, MinValue, MaxValue);
	if (!SliderTag.IsValid())
	{
		return;
	}

	FSettingsPicker* FoundRow = SettingsTableRowsInternal.Find(SliderTag.GetTagName());
	if (!FoundRow)
	{
		return;
	}

	const bool bChanged = !FMath::IsNearlyEqual(FoundRow->Slider.ChosenValue, NewValue);
	FoundRow->Slider.ChosenValue = NewValue;
	if (bChanged && !bIsSynchronizingSettingsInternal)
	{
		FoundRow->Slider.OnSetterFloat.ExecuteIfBound(NewValue);
	}

	if (USettingSlider* SettingSlider = GetSettingSubWidget<USettingSlider>(SliderTag))
	{
		SettingSlider->SetSliderValue(NewValue);
	}

	if (bChanged && !bIsSynchronizingSettingsInternal && !bSuppressChangeNotificationsInternal)
	{
		UpdateSettingsByTags(FoundRow->PrimaryData.SettingsToUpdate);
		OnAnySettingSet(FoundRow->PrimaryData);
	}
}

// Set new text
void USettingsWidget::SetSettingTextLine(const FSettingTag& TextLineTag, const FText& InValue)
{
	if (!TextLineTag.IsValid())
	{
		return;
	}

	FSettingsPicker* SettingsRowPtr = SettingsTableRowsInternal.Find(TextLineTag.GetTagName());
	if (!SettingsRowPtr)
	{
		return;
	}

	FSettingsPrimary& PrimaryRef = SettingsRowPtr->PrimaryData;
	FText& CaptionRef = PrimaryRef.Caption;
	if (CaptionRef.EqualTo(InValue))
	{
		return;
	}

	CaptionRef = InValue;
	if (!bIsSynchronizingSettingsInternal)
	{
		SettingsRowPtr->TextLine.OnSetterText.ExecuteIfBound(InValue);
	}

	if (USettingTextLine* SettingTextLine = Cast<USettingTextLine>(PrimaryRef.SettingSubWidget))
	{
		SettingTextLine->SetCaptionText(InValue);
	}

	if (!bIsSynchronizingSettingsInternal && !bSuppressChangeNotificationsInternal)
	{
		UpdateSettingsByTags(PrimaryRef.SettingsToUpdate);
		OnAnySettingSet(PrimaryRef);
	}
}

// Set new text for an input box
void USettingsWidget::SetSettingUserInput(const FSettingTag& UserInputTag, FName InValue)
{
	if (!UserInputTag.IsValid())
	{
		return;
	}

	FSettingsPicker* SettingsRowPtr = SettingsTableRowsInternal.Find(UserInputTag.GetTagName());
	if (!SettingsRowPtr)
	{
		return;
	}

	FSettingsUserInput& UserInputRef = SettingsRowPtr->UserInput;
	if (UserInputRef.UserInput.IsEqual(InValue)
	    || InValue.IsNone())
	{
		// Is not needed to update
		return;
	}

	if (UserInputRef.MaxCharactersNumber > 0)
	{
		// Limit the length of the string
		const FString NewValueStr = InValue.ToString().Left(UserInputRef.MaxCharactersNumber);
		InValue = *NewValueStr;
	}

	UserInputRef.UserInput = InValue;
	if (!bIsSynchronizingSettingsInternal)
	{
		UserInputRef.OnSetterName.ExecuteIfBound(InValue);
	}

	if (USettingUserInput* SettingUserInput = GetSettingSubWidget<USettingUserInput>(UserInputTag))
	{
		SettingUserInput->SetUserInputValue(InValue);
	}

	if (!bIsSynchronizingSettingsInternal && !bSuppressChangeNotificationsInternal)
	{
		UpdateSettingsByTags(SettingsRowPtr->PrimaryData.SettingsToUpdate);
		OnAnySettingSet(SettingsRowPtr->PrimaryData);
		PlayUIClickSFX();
	}
}

// Set new custom widget for setting by specified tag
void USettingsWidget::SetSettingCustomWidget(const FSettingTag& CustomWidgetTag, USettingCustomWidget* InCustomWidget)
{
	if (!CustomWidgetTag.IsValid())
	{
		return;
	}

	FSettingsPicker* SettingsRowPtr = SettingsTableRowsInternal.Find(CustomWidgetTag.GetTagName());
	if (!SettingsRowPtr)
	{
		return;
	}

	TWeakObjectPtr<USettingSubWidget>& CustomWidgetRef = SettingsRowPtr->PrimaryData.SettingSubWidget;
	if (CustomWidgetRef == InCustomWidget)
	{
		return;
	}

	CustomWidgetRef.Reset();
	CustomWidgetRef = InCustomWidget;
	if (!bIsSynchronizingSettingsInternal)
	{
		SettingsRowPtr->CustomWidget.OnSetterWidget.ExecuteIfBound(InCustomWidget);
	}

	if (!bIsSynchronizingSettingsInternal && !bSuppressChangeNotificationsInternal)
	{
		UpdateSettingsByTags(SettingsRowPtr->PrimaryData.SettingsToUpdate);
		OnAnySettingSet(SettingsRowPtr->PrimaryData);
	}
}

// Is called after any setting is changed
void USettingsWidget::OnAnySettingSet_Implementation(const FSettingsPrimary& SettingPrimaryRow)
{
	RefreshPendingChanges();

	if (SettingPrimaryRow.bApplyImmediately)
	{
		ApplySettings();
	}
}

/* ---------------------------------------------------
 *		Getters by setting types
 * --------------------------------------------------- */

/** Retrieve a specific setting row using a given tag.
 * @param Tag The tag used to find the setting row.
 * @param DataMember The member that holds the desired value. */
#define GET_SETTING_ROW(Tag, DataMember)                  \
	const FSettingsPicker& FoundRow = GetSettingRow(Tag); \
	if (!FoundRow.IsValid())                              \
	{                                                     \
		return;                                           \
	}                                                     \
	const auto& Data = FoundRow.DataMember;

/** Executes the common pattern of getting a value from a data structure.
 * @param Tag The tag used to find the setting row.
 * @param DataMember The member that holds the desired value.
 * @param ValueType The type of value to retrieve.
 * @param ValueExpression The expression to retrieve the value.
 * @param GetterExpression The expression to retrieve the getter delegate.
 * @param DefaultValue The default value to return if no value is found. */
#define GET_SETTING_VALUE(Tag, DataMember, ValueType, ValueExpression, GetterExpression, DefaultValue) \
	{                                                                                                  \
		const FSettingsPicker& FoundRow = GetSettingRow(Tag);                                          \
		ValueType Value = DefaultValue;                                                                \
		if (FoundRow.IsValid())                                                                        \
		{                                                                                              \
			const auto& Data = FoundRow.DataMember;                                                    \
			Value = ValueExpression;                                                                   \
			const auto& Getter = GetterExpression;                                                     \
			if (Getter.IsBound())                                                                      \
			{                                                                                          \
				Value = Getter.Execute();                                                              \
			}                                                                                          \
		}                                                                                              \
		return Value;                                                                                  \
	}

// Returns is a checkbox toggled
bool USettingsWidget::GetCheckboxValue(const FSettingTag& CheckboxTag) const
{
	GET_SETTING_VALUE(CheckboxTag, Checkbox, bool, Data.bIsSet, Data.OnGetterBool, false);
}

// Returns chosen member index of a combobox
int32 USettingsWidget::GetComboboxIndex(const FSettingTag& ComboboxTag) const
{
	GET_SETTING_VALUE(ComboboxTag, Combobox, int32, Data.ChosenMemberIndex, Data.OnGetterInt, 0);
}

// Get all members of a combobox
void USettingsWidget::GetComboboxMembers(const FSettingTag& ComboboxTag, TArray<FText>& OutMembers) const
{
	GET_SETTING_ROW(ComboboxTag, Combobox)
	OutMembers = Data.Members;
	Data.OnGetMembers.ExecuteIfBound(OutMembers);
}

// Get current value of a slider [0...1]
double USettingsWidget::GetSliderValue(const FSettingTag& SliderTag) const
{
	GET_SETTING_VALUE(SliderTag, Slider, double, Data.ChosenValue, Data.OnGetterFloat, 0.f);
}

// Get current text of a simple text widget
void USettingsWidget::GetTextLineValue(const FSettingTag& TextLineTag, FText& OutText) const
{
	GET_SETTING_ROW(TextLineTag, PrimaryData)
	OutText = Data.Caption;
	FoundRow.TextLine.OnGetterText.ExecuteIfBound(OutText);
}

// Get current input name of the text input
FName USettingsWidget::GetUserInputValue(const FSettingTag& UserInputTag) const
{
	GET_SETTING_VALUE(UserInputTag, UserInput, FName, Data.UserInput, Data.OnGetterName, NAME_None);
}

// Get custom widget of the setting by specified tag
USettingCustomWidget* USettingsWidget::GetCustomWidget(const FSettingTag& CustomWidgetTag) const
{
	GET_SETTING_VALUE(CustomWidgetTag, CustomWidget, USettingCustomWidget*, Cast<USettingCustomWidget>(FoundRow.PrimaryData.SettingSubWidget.Get()), Data.OnGetterWidget, nullptr);
}

// Get setting widget object by specified tag
USettingSubWidget* USettingsWidget::GetSettingSubWidget(const FSettingTag& SettingTag) const
{
	const FSettingsPrimary& PrimaryData = GetSettingRow(SettingTag).PrimaryData;
	return PrimaryData.IsValid() ? PrimaryData.SettingSubWidget.Get() : nullptr;
}

/* ---------------------------------------------------
 *		Style
 * --------------------------------------------------- */

// Returns the size of the Settings widget on the screen
FVector2D USettingsWidget::GetSettingsSize() const
{
	const FVector2D PercentSize = USettingsDataAsset::Get().GetSettingsPercentSize();

	UObject* WorldContextObject = GetOwningPlayer();
	const FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(WorldContextObject);
	const float ViewportScale = UWidgetLayoutLibrary::GetViewportScale(WorldContextObject);

	const FVector2D NewViewportSize = ViewportSize * PercentSize;
	return ViewportScale ? NewViewportSize / ViewportScale : NewViewportSize;
}

// Returns the size of specified category on the screen
FVector2D USettingsWidget::GetSubWidgetsSize(int32 SectionsBitmask) const
{
	if (!SectionsBitmask)
	{
		return FVector2D::ZeroVector;
	}

	TArray<const UWidget*> DesiredWidgets;

	constexpr int32 HeaderAlignment = static_cast<int32>(EMyVerticalAlignment::Header);
	if (HeaderAlignment & SectionsBitmask)
	{
		DesiredWidgets.Emplace(HeaderVerticalBox);
	}

	constexpr int32 ContentAlignment = static_cast<int32>(EMyVerticalAlignment::Content);
	if (ContentAlignment & SectionsBitmask)
	{
		DesiredWidgets.Emplace(ContentHorizontalBox);
	}

	constexpr int32 FooterAlignment = static_cast<int32>(EMyVerticalAlignment::Footer);
	if (FooterAlignment & SectionsBitmask)
	{
		DesiredWidgets.Emplace(FooterVerticalBox);
	}

	FVector2D SubWidgetsHeight = FVector2D::ZeroVector;
	for (const UWidget* DesiredWidgetIt : DesiredWidgets)
	{
		if (DesiredWidgetIt)
		{
			const FVector2D SubWidgetHeight = DesiredWidgetIt->GetDesiredSize();
			ensureAlwaysMsgf(!SubWidgetHeight.IsZero(), TEXT("ASSERT: 'SubWidgetHeight' is zero, can't get the size of subwidget, most likely widget is not initialized yet, call ForceLayoutPrepass()"));
			SubWidgetsHeight += SubWidgetHeight;
		}
	}
	return SubWidgetsHeight;
}

// Returns the height of a setting scrollbox on the screen
float USettingsWidget::GetScrollBoxHeight() const
{
	const USettingsDataAsset& SettingsData = USettingsDataAsset::Get();

	// The widget size
	const FVector2D SettingsSize = GetSettingsSize();

	// Margin size
	constexpr int32 Margins = static_cast<int32>(EMyVerticalAlignment::Margins);
	const FVector2D MarginsSize = GetSubWidgetsSize(Margins);

	// Additional padding sizes
	float Paddings = 0.f;
	const FMargin SettingsPadding = SettingsData.GetSettingsPadding();
	Paddings += SettingsPadding.Top + SettingsPadding.Bottom;
	const FMargin ScrollBoxPadding = SettingsData.GetColumnPadding();
	Paddings += ScrollBoxPadding.Top + ScrollBoxPadding.Bottom;

	const float ScrollBoxHeight = (SettingsSize - MarginsSize).Y - Paddings;

	// Scale scrollbox
	const float PercentSize = FMath::Clamp(SettingsData.GetScrollboxPercentHeight(), 0.f, 1.f);
	const float ScaledHeight = ScrollBoxHeight * PercentSize;

	return ScaledHeight;
}

// Is blueprint-event called that returns the style brush by specified button state
FSlateBrush USettingsWidget::GetButtonBrush(ESettingsButtonState State)
{
	const USettingsDataAsset& SettingsDataAsset = USettingsDataAsset::Get();
	const FMiscThemeData& MiscThemeData = SettingsDataAsset.GetMiscThemeData();
	const FButtonThemeData& ButtonThemeData = SettingsDataAsset.GetButtonThemeData();

	FSlateColor SlateColor;
	switch (State)
	{
		case ESettingsButtonState::Normal:
			SlateColor = MiscThemeData.ThemeColorNormal;
			break;
		case ESettingsButtonState::Hovered:
			SlateColor = MiscThemeData.ThemeColorHover;
			break;
		case ESettingsButtonState::Pressed:
			SlateColor = MiscThemeData.ThemeColorExtra;
			break;
		default:
			SlateColor = FLinearColor::White;
	}

	FSlateBrush SlateBrush;
	SlateBrush.TintColor = SlateColor;
	SlateBrush.DrawAs = ButtonThemeData.DrawAs;
	SlateBrush.Margin = ButtonThemeData.Margin;
	SlateBrush.SetImageSize(ButtonThemeData.Size);
	SlateBrush.SetResourceObject(ButtonThemeData.Texture);

	return SlateBrush;
}

// Returns the horizontal footer action panel, with the original vertical box as a safe fallback
UPanelWidget* USettingsWidget::GetFooterPanel() const
{
	return FooterButtonBoxInternal ? Cast<UPanelWidget>(FooterButtonBoxInternal) : Cast<UPanelWidget>(FooterVerticalBox);
}

/* ---------------------------------------------------
 *		Protected functions
 * --------------------------------------------------- */

// Called after the underlying slate widget is constructed
void USettingsWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (USettingsDataAsset::Get().IsAutoConstruct())
	{
		TryConstructSettings();
	}

	BindOnSettingsDataRegistryChanged();
}

// Called when the widget is removed from the viewport
void USettingsWidget::NativeDestruct()
{
	Super::NativeDestruct();

	RemoveAllSettings();
}

FReply USettingsWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		CancelSettings();
		return FReply::Handled();
	}

	if (InKeyEvent.GetKey() == EKeys::Enter)
	{
		AcceptSettings();
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

// Is called right after the game was started and windows size is set to construct settings
void USettingsWidget::OnViewportResizedWhenInit(FViewport* Viewport, uint32 Index)
{
	if (FViewport::ViewportResizedEvent.IsBoundToObject(this))
	{
		FViewport::ViewportResizedEvent.RemoveAll(this);
	}

	ConstructSettings();
}

// Construct all settings from the settings data table
void USettingsWidget::ConstructSettings()
{
	if (IsSettingsWidgetConstructed())
	{
		// Settings are already constructed
		return;
	}

	CacheTable();
	UpdateSettingsWindowLayout();

	// BP implementation to cache some data before creating subwidgets
	OnConstructSettings();
	EnsureFooterButtonBox();
		UHorizontalBox* RootContentHorizontalBox = ContentHorizontalBox;
		BuildSettingsPageLayout();

	FGameplayTagContainer AddedSettings;
	for (const FName& OrderedTag : OrderedSettingTagsInternal)
	{
		FSettingsPicker* SettingPtr = SettingsTableRowsInternal.Find(OrderedTag);
		if (!SettingPtr)
		{
			continue;
		}
		FSettingsPicker& SettingRef = *SettingPtr;
		BindSetting(SettingRef);
		AddSetting(SettingRef);
		AddedSettings.AddTag(SettingRef.PrimaryData.Tag);
	}
	ContentHorizontalBox = RootContentHorizontalBox;
	ActiveSettingsPageContentInternal = nullptr;

	UpdateSettingsByTags(AddedSettings, /*bLoadFromConfig*/ true);

	UpdateScrollBoxesHeight();

	ApplySettings();
	CaptureCurrentSettingValues();
}

// Internal function to cache setting rows from Settings Data Table
void USettingsWidget::CacheTable()
{
	TMap<FName, FSettingsPicker> SettingRows;
	USettingsUtilsLibrary::GenerateAllSettingRows(/*Out*/ SettingRows);
	if (!ensureMsgf(!SettingRows.IsEmpty(), TEXT("ASSERT: 'SettingRows' are empty")))
	{
		return;
	}

	// Reset values if currently are set
	SettingsTableRowsInternal.Empty();
	SettingsTableRowsInternal.Reserve(SettingRows.Num());
	for (const TTuple<FName, FSettingsPicker>& SettingRowIt : SettingRows)
	{
		const FSettingsPicker& SettingsPicker = SettingRowIt.Value;
		SettingsTableRowsInternal.Emplace(SettingRowIt.Key, SettingsPicker);
	}

	SettingRows.GetKeys(OrderedSettingTagsInternal);
	OrderedSettingTagsInternal.Sort([this](const FName& Left, const FName& Right)
	{
		const FSettingsPicker* LeftRow = SettingsTableRowsInternal.Find(Left);
		const FSettingsPicker* RightRow = SettingsTableRowsInternal.Find(Right);
		const int32 LeftPriority = LeftRow ? LeftRow->PrimaryData.SortPriority : 0;
		const int32 RightPriority = RightRow ? RightRow->PrimaryData.SortPriority : 0;
		return LeftPriority == RightPriority ? Left.LexicalLess(Right) : LeftPriority < RightPriority;
	});
}

// Clears all added settings
void USettingsWidget::RemoveAllSettings()
{
	for (TTuple<FName, FSettingsPicker>& RowIt : SettingsTableRowsInternal)
	{
		USettingSubWidget* SubWidget = RowIt.Value.PrimaryData.SettingSubWidget.Get();
		if (IsValid(SubWidget))
		{
			FSWCWidgetUtilsLibrary::DestroyWidget(*SubWidget);
		}
	}
	SettingsTableRowsInternal.Empty();
	OrderedSettingTagsInternal.Empty();
	OpenedSettingValuesInternal.Empty();
	bHasPendingChangesInternal = false;

	for (USettingColumn* ColumnIt : ColumnsInternal)
	{
		if (ensureMsgf(ColumnIt, TEXT("ASSERT: [%i] %s:\n'ColumnIt' is not valid!"), __LINE__, *FString(__FUNCTION__)))
		{
			FSWCWidgetUtilsLibrary::DestroyWidget(*ColumnIt);
		}
	}
	ColumnsInternal.Empty();
		if (SettingsPageNavigationInternal)
		{
			SettingsPageNavigationInternal->ClearChildren();
			SettingsPageNavigationInternal->SetVisibility(ESlateVisibility::Collapsed);
			if (UPanelWidget* NavigationContainer = SettingsPageNavigationInternal->GetParent())
			{
				NavigationContainer->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		if (SettingsPageSwitcherInternal)
		{
			SettingsPageSwitcherInternal->ClearChildren();
			SettingsPageSwitcherInternal->SetVisibility(ESlateVisibility::Collapsed);
		}
		ActiveSettingsPageContentInternal = nullptr;
		SettingsPageContentByIdInternal.Empty();
		SettingsPageIndexByIdInternal.Empty();
		SettingsPageColumnCountsInternal.Empty();
		SettingPageIdByTagInternal.Empty();
		SettingColumnIndexByTagInternal.Empty();
		ActiveSettingsPageIdInternal = NAME_None;
		bHasCategoryPagesInternal = false;
}

// Is called when In-Game menu became opened or closed
void USettingsWidget::OnToggleSettings(bool bIsVisible)
{
	PlayUIClickSFX();

	if (OnToggledSettings.IsBound())
	{
		OnToggledSettings.Broadcast(bIsVisible);
	}
}

void USettingsWidget::EnsureFooterButtonBox()
{
	if (FooterButtonBoxInternal || !FooterVerticalBox || !WidgetTree)
	{
		return;
	}

	FooterButtonBoxInternal = Cast<UHorizontalBox>(WidgetTree->FindWidget(TEXT("SettingsFooterActions")));
	if (FooterButtonBoxInternal)
	{
		return;
	}
	FooterButtonBoxInternal = WidgetTree->ConstructWidget<UHorizontalBox>(
		UHorizontalBox::StaticClass(), TEXT("SettingsFooterActions"));
	if (UVerticalBoxSlot* FooterSlot = FooterVerticalBox->AddChildToVerticalBox(FooterButtonBoxInternal))
	{
		FooterSlot->SetHorizontalAlignment(HAlign_Fill);
		FooterSlot->SetVerticalAlignment(VAlign_Center);
		FooterSlot->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}
}

void USettingsWidget::CaptureCurrentSettingValues()
{
	OpenedSettingValuesInternal.Empty();
	for (const FName& OrderedTag : OrderedSettingTagsInternal)
	{
		const FSettingsPicker* Setting = SettingsTableRowsInternal.Find(OrderedTag);
		const FSettingsDataBase* ChosenData = Setting ? Setting->GetChosenSettingsData() : nullptr;
		if (!Setting || !ChosenData || !ChosenData->CanUpdateSetting())
		{
			continue;
		}

		FString Value;
		ChosenData->GetSettingValue(*this, Setting->PrimaryData.Tag, Value);
		OpenedSettingValuesInternal.Add(OrderedTag, MoveTemp(Value));
	}
}

void USettingsWidget::RefreshPendingChanges()
{
	bool bIsDirty = false;
	for (const TTuple<FName, FString>& OpenedValue : OpenedSettingValuesInternal)
	{
		const FSettingsPicker* Setting = SettingsTableRowsInternal.Find(OpenedValue.Key);
		const FSettingsDataBase* ChosenData = Setting ? Setting->GetChosenSettingsData() : nullptr;
		if (!Setting || !ChosenData || !ChosenData->CanUpdateSetting())
		{
			continue;
		}

		FString CurrentValue;
		ChosenData->GetSettingValue(*this, Setting->PrimaryData.Tag, CurrentValue);
		if (CurrentValue != OpenedValue.Value)
		{
			bIsDirty = true;
			break;
		}
	}
	SetHasPendingChanges(bIsDirty);
}

void USettingsWidget::SetHasPendingChanges(bool bNewValue)
{
	if (bHasPendingChangesInternal == bNewValue)
	{
		return;
	}

	bHasPendingChangesInternal = bNewValue;
	OnPendingSettingsChanged.Broadcast(bHasPendingChangesInternal);
}

void USettingsWidget::HideSettings()
{
	SetVisibility(ESlateVisibility::Collapsed);
	OnToggleSettings(false);
	OnCloseSettings();
}

// Bind and set static object delegate
bool USettingsWidget::TryBindOwner(FSettingsPrimary& Primary)
{
	const UObject* FoundContextObj = nullptr;
	const FSettingFunctionPicker& Owner = Primary.Owner;
	if (Owner.IsValid())
	{
		Primary.OwnerFunc.BindUFunction(Owner.FunctionClass->GetDefaultObject(), Owner.FunctionName);
		FoundContextObj = Primary.GetSettingOwner(this);
	}

	if (!FoundContextObj)
	{
		if (Owner.IsValid())
		{
			// Static context function is set, but returning object is null,
			// most likely such object is not initialized yet,
			// defer binding to try to rebind it later
			DeferredBindingsInternal.AddTag(Primary.Tag);
		}

		return false;
	}

	const UClass* ContextClass = FoundContextObj->GetClass();
	checkf(ContextClass, TEXT("ERROR: [%i] %s:\n'ContextClass' is null!"), __LINE__, *FString(__FUNCTION__));

	// Cache all functions that are contained in returned object
	Primary.OwnerFunctionList.Reset();
	for (TFieldIterator<UFunction> It(ContextClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const UFunction* FunctionIt = *It;
		if (!FunctionIt)
		{
			continue;
		}

		const FName FunctionNameIt = FunctionIt->GetFName();
		if (!FunctionNameIt.IsNone())
		{
			Primary.OwnerFunctionList.Emplace(FunctionNameIt);
		}
	}

	return true;
}

// Creates new widget based on specified setting class and sets it to specified primary data
USettingSubWidget* USettingsWidget::CreateSettingSubWidget(FSettingsPrimary& InOutPrimary, const TSubclassOf<USettingSubWidget> SettingSubWidgetClass)
{
	if (!SettingSubWidgetClass)
	{
		return nullptr;
	}

	USettingSubWidget* SettingSubWidget = CreateWidget<USettingSubWidget>(this, SettingSubWidgetClass);
	InOutPrimary.SettingSubWidget = SettingSubWidget;
	SettingSubWidget->SetSettingsWidget(this);
	SettingSubWidget->SetSettingPrimaryRow(InOutPrimary);
	SettingSubWidget->SetLineHeight(InOutPrimary.LineHeight);
	SettingSubWidget->SetCaptionText(InOutPrimary.Caption);

	return SettingSubWidget;
}

// Automatically sets the height for all scrollboxes in the Settings
void USettingsWidget::UpdateScrollBoxesHeight()
{
	ForceLayoutPrepass(); // Call it to make GetSettingsHeight work since it is called during widget construction
	const float ScrollBoxHeight = GetScrollBoxHeight();

	for (const USettingColumn* ColumnIt : ColumnsInternal)
	{
		USizeBox* SizeBoxWidget = ColumnIt ? ColumnIt->GetSizeBoxWidget() : nullptr;
		if (SizeBoxWidget)
		{
			SizeBoxWidget->SetMaxDesiredHeight(ScrollBoxHeight);
		}
	}
}

// Constructs settings if viewport is ready otherwise Wait until viewport become initialized
void USettingsWidget::TryConstructSettings()
{
	auto IsViewportInitialized = []() -> bool
	{
		UGameViewportClient* GameViewport = GEngine ? GEngine->GameViewport : nullptr;
		FViewport* Viewport = GameViewport ? GameViewport->Viewport : nullptr;
		if (!Viewport)
		{
			return false;
		}

		auto IsZeroViewportSize = [Viewport]
		{
			return Viewport->GetSizeXY() == FIntPoint::ZeroValue;
		};

		if (IsZeroViewportSize())
		{
			// Try update its value by mouse enter event
			GameViewport->MouseEnter(Viewport, FIntPoint::ZeroValue.X, FIntPoint::ZeroValue.Y);
			return !IsZeroViewportSize();
		}

		return true;
	};

	if (IsViewportInitialized())
	{
		ConstructSettings();
	}
	else if (!FViewport::ViewportResizedEvent.IsBoundToObject(this))
	{
		FViewport::ViewportResizedEvent.AddUObject(this, &ThisClass::OnViewportResizedWhenInit);
	}
}

// Display settings on UI
void USettingsWidget::OpenSettings()
{
	if (IsVisible())
	{
		// Is already shown
		return;
	}

	TryConstructSettings();
	UpdateSettingsWindowLayout();

	TryRebindDeferredContexts();

	UpdateAllSettings();
	CaptureCurrentSettingValues();
	SetHasPendingChanges(false);

	SetVisibility(ESlateVisibility::Visible);

	OnToggleSettings(true);

	TryFocusOnUI();

	OnOpenSettings();
}

// Backwards-compatible close behavior: commit current values
void USettingsWidget::CloseSettings()
{
	if (!IsVisible()
	    && !IsHovered())
	{
		// Widget is already closed
		return;
	}

	AcceptSettings();
}

void USettingsWidget::AcceptSettings()
{
	if (!IsVisible() && !IsHovered())
	{
		return;
	}

	SaveSettings();
	HideSettings();
}

void USettingsWidget::CancelSettings()
{
	if (!IsVisible() && !IsHovered())
	{
		return;
	}

	{
		TGuardValue<bool> NotificationGuard(bSuppressChangeNotificationsInternal, true);
		for (const FName& OrderedTag : OrderedSettingTagsInternal)
		{
			const FString* OpenedValue = OpenedSettingValuesInternal.Find(OrderedTag);
			FSettingsPicker* Setting = SettingsTableRowsInternal.Find(OrderedTag);
			FSettingsDataBase* ChosenData = Setting ? Setting->GetChosenSettingsData() : nullptr;
			if (OpenedValue && Setting && ChosenData && ChosenData->CanUpdateSetting())
			{
				ChosenData->SetSettingValue(*this, Setting->PrimaryData.Tag, *OpenedValue);
			}
		}
	}

	ApplySettings();
	UpdateAllSettings(false);
	SetHasPendingChanges(false);
	HideSettings();
}

void USettingsWidget::RestoreDefaultSettings()
{
	UGameUserSettings* GameUserSettings = USettingsUtilsLibrary::GetGameUserSettings();
	if (!GameUserSettings)
	{
		return;
	}

	GameUserSettings->SetToDefaults();
	UpdateAllSettings(false);
	ApplySettings();
	RefreshPendingChanges();
}

// Flip-flop opens and closes the Settings menu
void USettingsWidget::ToggleSettings()
{
	if (IsVisible())
	{
		CloseSettings();
	}
	else
	{
		OpenSettings();
	}
}

// Is called on opening to focus the widget on UI if allowed
void USettingsWidget::TryFocusOnUI()
{
	if (!USettingsDataAsset::Get().IsAutoFocusOnOpen())
	{
		return;
	}

	APlayerController* PlayerController = GetOwningPlayer();
	if (!ensureMsgf(PlayerController, TEXT("ASSERT: [%i] %s:\n'PlayerController' is not valid!"), __LINE__, *FString(__FUNCTION__)))
	{
		return;
	}

	FInputModeGameAndUI GameAndUI;
	SetIsFocusable(true);
	GameAndUI.SetWidgetToFocus(TakeWidget());
	PlayerController->SetInputMode(GameAndUI);
	PlayerController->SetShowMouseCursor(true);
	PlayerController->bEnableClickEvents = true;
	PlayerController->bEnableMouseOverEvents = true;
	SetKeyboardFocus();
}

/* ---------------------------------------------------
 *		Bind by setting types
 * --------------------------------------------------- */

// Bind setting to specified Get/Set delegates, so both methods will be called
bool USettingsWidget::BindSetting(FSettingsPicker& Setting)
{
	FSettingsDataBase* ChosenData = Setting.GetChosenSettingsData();
	if (!ChosenData)
	{
		return false;
	}

	if (TryBindOwner(Setting.PrimaryData))
	{
		ChosenData->BindSetting(*this, Setting.PrimaryData);
		return true;
	}

	return false;
}

/**
 * Macro to create and bind a UI widget.
 * @param Primary				Primary settings for the widget
 * @param Data					Data structure containing widget properties
 * @param GetterFunction		The getter function to bind
 * @param SetterFunction		The setter function to bind
 * @param AdditionalFunctionCalls Any additional function calls needed for specific widgets
 */
#define BIND_SETTING(Primary, Data, GetterFunction, SetterFunction)                 \
	do                                                                              \
	{                                                                               \
		if (UObject* OwnerObject = Primary.GetSettingOwner(this))                   \
		{                                                                           \
			const FName GetterFunctionName = Primary.Getter.FunctionName;           \
			if (Primary.OwnerFunctionList.Contains(GetterFunctionName))             \
			{                                                                       \
				Data.GetterFunction.BindUFunction(OwnerObject, GetterFunctionName); \
			}                                                                       \
			const FName SetterFunctionName = Primary.Setter.FunctionName;           \
			if (Primary.OwnerFunctionList.Contains(SetterFunctionName))             \
			{                                                                       \
				Data.SetterFunction.BindUFunction(OwnerObject, SetterFunctionName); \
			}                                                                       \
		}                                                                           \
	} while (0)

// Bind button to own Get/Set delegates
void USettingsWidget::BindButton(const FSettingsPrimary& Primary, FSettingsButton& Data)
{
	BIND_SETTING(Primary, Data, OnButtonPressed, OnButtonPressed);
}

// Bind checkbox to own Get/Set delegates
void USettingsWidget::BindCheckbox(const FSettingsPrimary& Primary, FSettingsCheckbox& Data)
{
	BIND_SETTING(Primary, Data, OnGetterBool, OnSetterBool);
}

// Bind combobox to own Get/Set delegates
void USettingsWidget::BindCombobox(const FSettingsPrimary& Primary, FSettingsCombobox& Data)
{
	BIND_SETTING(Primary, Data, OnGetterInt, OnSetterInt);

	if (UObject* OwnerObject = Primary.GetSettingOwner(this))
	{
		const FName GetMembersFunctionName = Data.GetMembers.FunctionName;
		if (Primary.OwnerFunctionList.Contains(GetMembersFunctionName))
		{
			Data.OnGetMembers.BindUFunction(OwnerObject, GetMembersFunctionName);
			Data.OnGetMembers.ExecuteIfBound(Data.Members);
		}

		const FName SetMembersFunctionName = Data.SetMembers.FunctionName;
		if (Primary.OwnerFunctionList.Contains(SetMembersFunctionName))
		{
			Data.OnSetMembers.BindUFunction(OwnerObject, SetMembersFunctionName);
			Data.OnSetMembers.ExecuteIfBound(Data.Members);
		}
	}
}

// Bind slider to own Get/Set delegates
void USettingsWidget::BindSlider(const FSettingsPrimary& Primary, FSettingsSlider& Data)
{
	BIND_SETTING(Primary, Data, OnGetterFloat, OnSetterFloat);
}

// Bind simple text to own Get/Set delegates
void USettingsWidget::BindTextLine(const FSettingsPrimary& Primary, FSettingsTextLine& Data)
{
	BIND_SETTING(Primary, Data, OnGetterText, OnSetterText);
}

// Bind text input to own Get/Set delegates
void USettingsWidget::BindUserInput(const FSettingsPrimary& Primary, FSettingsUserInput& Data)
{
	BIND_SETTING(Primary, Data, OnGetterName, OnSetterName);
}

// Bind custom widget to own Get/Set delegates
void USettingsWidget::BindCustomWidget(const FSettingsPrimary& Primary, FSettingsCustomWidget& Data)
{
	BIND_SETTING(Primary, Data, OnGetterWidget, OnSetterWidget);
}

// Attempts to rebind those Settings that failed to bind their Getter/Setter functions on initial construct
void USettingsWidget::TryRebindDeferredContexts()
{
	if (DeferredBindingsInternal.IsEmpty())
	{
		// Nothing to rebind, we are done
		return;
	}

	FGameplayTagContainer ReboundSettings;
	for (const FGameplayTag& TagIt : DeferredBindingsInternal)
	{
		FSettingsPicker* FoundRowPtr = TagIt.IsValid() ? SettingsTableRowsInternal.Find(TagIt.GetTagName()) : nullptr;
		if (FoundRowPtr
		    && BindSetting(*FoundRowPtr))
		{
			ReboundSettings.AddTagFast(TagIt);
		}
	}

	if (!ReboundSettings.IsEmpty())
	{
		// Some settings were successfully rebound, remove them from the deferred list and update them
		DeferredBindingsInternal.RemoveTags(ReboundSettings);
		UpdateSettingsByTags(ReboundSettings, /*bLoadFromConfig*/ true);
	}
}

// Add setting on UI.
void USettingsWidget::AddSetting(FSettingsPicker& Setting)
{
	const FSettingsDataBase* ChosenData = Setting.GetChosenSettingsData();
	if (!ChosenData)
	{
		return;
	}

	FSettingsPrimary& PrimaryData = Setting.PrimaryData;
		if (bHasCategoryPagesInternal)
		{
			const FName SettingTagName = PrimaryData.Tag.GetTagName();
			FName PageId = SettingPageIdByTagInternal.FindRef(SettingTagName);
			if (PageId.IsNone())
			{
				PageId = SettingsPageIndexByIdInternal.Contains(PrimaryData.PageId) ? PrimaryData.PageId : FName(TEXT("Other"));
				if (!SettingsPageIndexByIdInternal.Contains(PageId) && !SettingsPageIndexByIdInternal.IsEmpty())
				{
					PageId = SettingsPageIndexByIdInternal.CreateConstIterator()->Key;
				}
				SettingPageIdByTagInternal.Add(SettingTagName, PageId);
			}
			ActiveSettingsPageContentInternal = SettingsPageContentByIdInternal.FindRef(PageId);
			if (ActiveSettingsPageContentInternal)
			{
				ContentHorizontalBox = ActiveSettingsPageContentInternal;
			}
		}

	if (ChosenData->GetVerticalAlignment() == EMyVerticalAlignment::Content)
		{
			if (bHasCategoryPagesInternal)
			{
				const FName PageId = SettingPageIdByTagInternal.FindChecked(PrimaryData.Tag.GetTagName());
				int32& PageColumnEnd = SettingsPageColumnCountsInternal.FindChecked(PageId);
				if (PageColumnEnd == 0 || PrimaryData.bStartOnNextColumn)
				{
					AddColumn(ColumnsInternal.Num());
					PageColumnEnd = ColumnsInternal.Num();
				}
				SettingColumnIndexByTagInternal.Add(PrimaryData.Tag.GetTagName(), PageColumnEnd - 1);
			}
			else if (ColumnsInternal.IsEmpty() || PrimaryData.bStartOnNextColumn)
			{
				AddColumn(ColumnsInternal.Num());
			}
		}

	USettingSubWidget* SettingSubWidget = CreateSettingSubWidget(PrimaryData, ChosenData->GetSubWidgetClass());
	checkf(SettingSubWidget, TEXT("ERROR: [%i] %s:\n'SettingSubWidget' is null!"), __LINE__, *FString(__FUNCTION__));
	SettingSubWidget->OnAddSetting(Setting);
}

/*********************************************************************************************
 * Columns builder
 ********************************************************************************************* */

// Returns the index of column for a Setting by specified tag or -1 if not found
int32 USettingsWidget::GetColumnIndexBySetting(const FSettingTag& SettingTag) const
{
	if (const int32* CachedColumnIndex = SettingColumnIndexByTagInternal.Find(SettingTag.GetTagName()))
	{
		return *CachedColumnIndex;
	}
	int32 ColumnIndex = 0;
	for (const FName& OrderedTag : OrderedSettingTagsInternal)
	{
		const FSettingsPicker* Row = SettingsTableRowsInternal.Find(OrderedTag);
		if (!Row)
		{
			continue;
		}
		const FSettingsPrimary& PrimaryData = Row->PrimaryData;
		if (PrimaryData.bStartOnNextColumn)
		{
			++ColumnIndex;
		}

		if (PrimaryData.Tag == SettingTag)
		{
			return ColumnIndex;
		}
	}

	return INDEX_NONE;
}

// Creates new column on specified index
void USettingsWidget::AddColumn(int32 ColumnIndex)
{
	USettingColumn* NewColumn = CreateWidget<USettingColumn>(this, USettingsDataAsset::Get().GetColumnClass());
	NewColumn->SetSettingsWidget(this);
	ColumnIndex = FMath::Clamp(ColumnIndex, 0, ColumnsInternal.Num());
	ColumnsInternal.Insert(NewColumn, ColumnIndex);
	NewColumn->OnAddSetting(FSettingsPicker());
}

/*********************************************************************************************
 * Multiple Data Tables support
 ********************************************************************************************* */

// Is called when the Settings Data Registry is changed
void USettingsWidget::OnSettingsDataRegistryChanged_Implementation(class UDataRegistry* SettingsDataRegistry)
{
	const UWorld* World = GetWorld();
	const APlayerController* PC = GetOwningPlayer();
	if (!World || World->bIsTearingDown
	    || !PC
	    || !IsInViewport()
	    || !SettingsDataRegistry || SettingsDataRegistry->GetLowestAvailability() == EDataRegistryAvailability::DoesNotExist)
	{
		// The game was ended or no data registry is set
		return;
	}

#if WITH_EDITOR
	if (GEditor && GEditor->ShouldEndPlayMap())
	{
		return;
	}
#endif

	// Perfectly, we should insert new settings here,
	// But inserting anything in between to scrollbox is not supported by UE at all
	// So, clear all settings first
	RemoveAllSettings();
	ConstructSettings();
}

void USettingsWidget::BindOnSettingsDataRegistryChanged()
{
	UDataRegistry* SettingsDataRegistry = USettingsDataAsset::Get().GetSettingsDataRegistry();
	checkf(SettingsDataRegistry, TEXT("ERROR: [%i] %s:\n'SettingsDataRegistry' is not set in Project Settings!"), __LINE__, *FString(__FUNCTION__));
	FDataRegistryCacheVersionCallback& SettingsDataRegistryDelegate = SettingsDataRegistry->OnCacheVersionInvalidated();
	if (!SettingsDataRegistryDelegate.IsBoundToObject(this))
	{
		SettingsDataRegistryDelegate.AddUObject(this, &ThisClass::OnSettingsDataRegistryChanged);
	}
}

void USettingsWidget::SetActiveSettingsPage(int32 PageIndex)
{
	if (!SettingsPageSwitcherInternal || SettingsPageIndexByIdInternal.IsEmpty())
	{
		return;
	}
	
	const int32 ActivePageIndex = FMath::Clamp(PageIndex, 0, SettingsPageIndexByIdInternal.Num() - 1);
	const int32 PreviousPageIndex = SettingsPageSwitcherInternal->GetActiveWidgetIndex();
	const FMiscThemeData& MiscThemeData = USettingsDataAsset::Get().GetMiscThemeData();
	
	FLinearColor ActiveTint = MiscThemeData.TextHeaderColor.GetSpecifiedColor() * 0.2f;
	ActiveTint.A = 1.0f;
	
	const auto SetNavigationButtonActive = [this, &MiscThemeData, &ActiveTint](int32 NavigationIndex, bool bIsActive)
	{
		if (!SettingsPageNavigationInternal)
		{
			return;
		}
		USettingsPageNavigationButton* NavigationButton = Cast<USettingsPageNavigationButton>(
			SettingsPageNavigationInternal->GetChildAt(NavigationIndex));
		if (!NavigationButton)
		{
			return;
		}
	
		FButtonStyle NavigationButtonStyle = NavigationButton->GetStyle();
		FSlateBrush NormalBrush = NavigationButtonStyle.Normal;
				FSlateBrush HoveredBrush = NavigationButtonStyle.Hovered;
				FSlateBrush PressedBrush = NavigationButtonStyle.Pressed;
				NormalBrush.TintColor = bIsActive ? FSlateColor (ActiveTint)
		: MiscThemeData.ThemeColorNormal;
				HoveredBrush.TintColor = MiscThemeData.ThemeColorHover;
				PressedBrush.TintColor = MiscThemeData.ThemeColorExtra;
			NavigationButtonStyle.SetNormal(NormalBrush);
			NavigationButtonStyle.SetHovered(HoveredBrush);
			NavigationButtonStyle.SetPressed(PressedBrush);
			NavigationButtonStyle.SetDisabled(NormalBrush);
		
		NavigationButton->SetStyle(NavigationButtonStyle);
		if (UTextBlock* CaptionText = Cast<UTextBlock>(NavigationButton->GetContent()))
		{
			CaptionText->SetColorAndOpacity(
				bIsActive ? MiscThemeData.TextHeaderColor : MiscThemeData.TextAndCaptionColor);
		}
	};
	
	if (PreviousPageIndex >= 0 && PreviousPageIndex != ActivePageIndex)
	{
		SetNavigationButtonActive(PreviousPageIndex, false);
	}
	SetNavigationButtonActive(ActivePageIndex, true);
	SettingsPageSwitcherInternal->SetActiveWidgetIndex(ActivePageIndex);
	if (const FName* ActivePageId = SettingsPageIndexByIdInternal.FindKey(ActivePageIndex))
	{
		ActiveSettingsPageIdInternal = *ActivePageId;
	}
}

void USettingsPageNavigationButton::HandleClicked()
{
	UPanelWidget* ParentPanel = GetParent();
	if (!ParentPanel)
	{
		return;
	}
	const int32 PageIndex = ParentPanel->GetChildIndex(this);
	if (PageIndex == INDEX_NONE)
	{
		return;
	}
	if (USettingsWidget* SettingsWidget = GetTypedOuter<USettingsWidget>())
	{
		SettingsWidget->SetActiveSettingsPage(PageIndex);
	}
}

bool USettingsWidget::BuildSettingsPageLayout()
{
	SettingsPageSwitcherInternal = nullptr;
	SettingsPageNavigationInternal = nullptr;
	ActiveSettingsPageContentInternal = nullptr;
	SettingsPageContentByIdInternal.Empty();
	SettingsPageIndexByIdInternal.Empty();
	SettingsPageColumnCountsInternal.Empty();
	SettingPageIdByTagInternal.Empty();
	SettingColumnIndexByTagInternal.Empty();
	bHasCategoryPagesInternal = false;
	
	if (!WidgetTree || !ContentHorizontalBox)
	{
		return false;
	}
	
	const FMiscThemeData& WindowTitleTheme = USettingsDataAsset::Get().GetMiscThemeData();
	if (UTextBlock* SettingsWindowTitle = Cast<UTextBlock>(
		WidgetTree->FindWidget(TEXT("SettingsWindowTitle"))))
	{
		SettingsWindowTitle->SetFont(WindowTitleTheme.TextHeaderFont);
		SettingsWindowTitle->SetColorAndOpacity(WindowTitleTheme.TextHeaderColor);
	}
	
	SettingsPageNavigationInternal = Cast<UVerticalBox>(
		WidgetTree->FindWidget(TEXT("SettingsPageNavigation")));
	SettingsPageSwitcherInternal = Cast<UWidgetSwitcher>(
		WidgetTree->FindWidget(TEXT("SettingsPageSwitcher")));
	const TArray<FSettingsPageDefinition>& PageDefinitions = USettingsDataAsset::Get().GetSettingsPages();
	if (!SettingsPageNavigationInternal || !SettingsPageSwitcherInternal || PageDefinitions.IsEmpty())
	{
		if (SettingsPageNavigationInternal)
		{
			SettingsPageNavigationInternal->ClearChildren();
			SettingsPageNavigationInternal->SetVisibility(ESlateVisibility::Collapsed);
			if (UPanelWidget* NavigationContainer = SettingsPageNavigationInternal->GetParent())
			{
				NavigationContainer->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		if (SettingsPageSwitcherInternal)
		{
			SettingsPageSwitcherInternal->ClearChildren();
			SettingsPageSwitcherInternal->SetVisibility(ESlateVisibility::Collapsed);
		}
		return false;
	}
	
	 UUserWidget* PageLayoutTemplate = Cast<UUserWidget>(
			WidgetTree->FindWidget(TEXT("SettingsPageTemplate")));
		USettingsPageNavigationButton* NavigationButtonTemplate =
			Cast<USettingsPageNavigationButton>(WidgetTree->FindWidget(TEXT("SettingsPageNavigationButtonTemplate")));
		UTextBlock* NavigationCaptionTemplate = NavigationButtonTemplate
			? Cast<UTextBlock>(NavigationButtonTemplate->GetContent())
			: nullptr;
		const UVerticalBoxSlot* NavigationButtonTemplateSlot = NavigationButtonTemplate
			? Cast<UVerticalBoxSlot>(NavigationButtonTemplate->Slot)
			: nullptr;
		if (!PageLayoutTemplate || !NavigationButtonTemplate || !NavigationCaptionTemplate)
		{
			return false;
		}
	
		UClass *const PageLayoutWidgetClass = PageLayoutTemplate->GetClass();
		const FButtonStyle NavigationButtonStyle = NavigationButtonTemplate->GetStyle();
		const FSlateFontInfo NavigationFont = NavigationCaptionTemplate->GetFont();
		const FSlateColor NavigationCaptionColor = NavigationCaptionTemplate->GetColorAndOpacity();
		const FMargin NavigationButtonPadding = NavigationButtonTemplateSlot
			? NavigationButtonTemplateSlot->GetPadding()
			: FMargin(0.0f, 0.0f, 0.0f, 8.0f);
		const EHorizontalAlignment NavigationButtonAlignment = NavigationButtonTemplateSlot
			? NavigationButtonTemplateSlot->GetHorizontalAlignment()
			: HAlign_Fill; SettingsPageNavigationInternal->ClearChildren();
	SettingsPageSwitcherInternal->ClearChildren();
	SettingsPageNavigationInternal->SetVisibility(ESlateVisibility::Visible);
	SettingsPageSwitcherInternal->SetVisibility(ESlateVisibility::Visible);
	if (UPanelWidget* NavigationContainer = SettingsPageNavigationInternal->GetParent())
	{
		NavigationContainer->SetVisibility(ESlateVisibility::Visible);
	}
	
	TArray<FSettingsPageDefinition> OrderedPages = PageDefinitions;
	OrderedPages.Sort([](const FSettingsPageDefinition& Left, const FSettingsPageDefinition& Right)
	{
		return Left.SortPriority == Right.SortPriority
			? Left.PageId.LexicalLess(Right.PageId)
			: Left.SortPriority < Right.SortPriority;
	});
	const FMiscThemeData& MiscTheme = USettingsDataAsset::Get().GetMiscThemeData();
	
	
	for (const FSettingsPageDefinition& PageDefinition : OrderedPages)
	{
		if (PageDefinition.PageId.IsNone() || SettingsPageIndexByIdInternal.Contains(PageDefinition.PageId))
		{
			continue;
		}
		const int32 PageIndex = SettingsPageIndexByIdInternal.Num();
			UUserWidget* PageLayout = CreateWidget<UUserWidget>(
				GetOwningPlayer(),
				PageLayoutWidgetClass);
						if(!PageLayout || !PageLayout->WidgetTree) {
							continue;
						}
						UWidgetTree* PageWidgetTree = PageLayout->WidgetTree;
			UTextBlock* PageTitleText = Cast<UTextBlock>(
				PageWidgetTree->FindWidget(TEXT("SettingsPageTitle")));
			UHorizontalBox* PageContent = Cast<UHorizontalBox>(
				PageWidgetTree->FindWidget(TEXT("SettingsPageContent")));
			 if (!PageTitleText || !PageContent)
					{
						continue;
					} PageTitleText->SetText(PageDefinition.Caption);
			
			SettingsPageSwitcherInternal->AddChild(PageLayout);
			SettingsPageIndexByIdInternal.Add(PageDefinition.PageId, PageIndex);
			SettingsPageContentByIdInternal.Add(PageDefinition.PageId, PageContent);
			SettingsPageColumnCountsInternal.Add(PageDefinition.PageId, 0);
	
		USettingsPageNavigationButton* NavigationButton = WidgetTree->ConstructWidget<USettingsPageNavigationButton>(
			USettingsPageNavigationButton::StaticClass(),
			*FString::Printf(TEXT("SettingsPageNavigationButton_%s"), *PageDefinition.PageId.ToString()));
		NavigationButton->SetStyle(NavigationButtonStyle);
		NavigationButton->OnClicked.AddDynamic(NavigationButton, &USettingsPageNavigationButton::HandleClicked);
		UTextBlock* CaptionText = WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(),
			*FString::Printf(TEXT("SettingsPageCaption_%s"), *PageDefinition.PageId.ToString()));
		CaptionText->SetText(PageDefinition.Caption);
		CaptionText->SetFont(NavigationFont);
		CaptionText->SetColorAndOpacity(NavigationCaptionColor);
		NavigationButton->AddChild(CaptionText);
		if (UVerticalBoxSlot* NavigationButtonSlot = SettingsPageNavigationInternal->AddChildToVerticalBox(NavigationButton))
		{
			NavigationButtonSlot->SetPadding(NavigationButtonPadding);
			NavigationButtonSlot->SetHorizontalAlignment(NavigationButtonAlignment);
		}
	}
	
	bHasCategoryPagesInternal = !SettingsPageIndexByIdInternal.IsEmpty();
	if (!bHasCategoryPagesInternal)
	{
		SettingsPageNavigationInternal->ClearChildren();
		SettingsPageSwitcherInternal->ClearChildren();
		SettingsPageNavigationInternal->SetVisibility(ESlateVisibility::Collapsed);
		SettingsPageSwitcherInternal->SetVisibility(ESlateVisibility::Collapsed);
		if (UPanelWidget* NavigationContainer = SettingsPageNavigationInternal->GetParent())
		{
			NavigationContainer->SetVisibility(ESlateVisibility::Collapsed);
		}
		return false;
	}
	
	SetActiveSettingsPage(0);
	return true;
}

void USettingsWidget::UpdateSettingsWindowLayout()
{
	

	if (UWidget* MenuVBox = WidgetTree->FindWidget(TEXT("Menu VBox")))
	{
		if (UOverlaySlot* MenuSlot = Cast<UOverlaySlot>(MenuVBox->Slot))
		{
			MenuSlot->SetPadding(USettingsDataAsset::Get().GetSettingsPadding());
		}
	}
}
