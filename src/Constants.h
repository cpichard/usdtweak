#pragma once
///
/// Constants used in the application
///

/// PI
constexpr float PI_F = 3.14159265;

/// Size of the main window when it opens.
constexpr int InitialWindowWidth = 1024;
constexpr int InitialWindowHeight = 1024;

/// Height of a row in the property editor
constexpr float TableRowDefaultHeight = 22.f;

/// Waiting time before a tooltip shows up
constexpr float TimeBeforeTooltip = 2.f; // 2 seconds

/// Predefined colors for the different widgets
#define ColorAttributeAuthored {1.0, 1.0, 1.0, 1.0}
#define ColorAttributeUnauthored {0.5, 0.5, 0.5, 1.0}
#define ColorAttributeRelationship {0.5, 0.5, 0.9, 1.0}
#define ColorAttributeConnection {0.5, 0.5, 0.9, 1.0}
#define ColorMiniButtonAuthored {0.0, 1.0, 0.0, 1.0}
#define ColorMiniButtonUnauthored {0.6, 0.6, 0.6, 1.0}
#define ColorTransparent {0.0, 0.0, 0.0, 0.0}
#define ColorPrimDefault {227.f/255.f, 227.f/255.f, 227.f/255.f, 1.0}
#define ColorPrimInactive {0.4, 0.4, 0.4, 1.0}
#define ColorPrimInstance {135.f/255.f, 206.f/255.f, 250.f/255.f, 1.0}
#define ColorPrimPrototype {118.f/255.f, 136.f/255.f, 217.f/255.f, 1.0}
#define ColorPrimHasComposition {222.f/255.f, 158.f/255.f, 46.f/255.f, 1.0}
#define ColorGreyish {0.5, 0.5, 0.5, 1.0}
#define ColorButtonHighlight {0.5, 0.7, 0.5, 0.7}
#define ColorEditableWidgetBg {0.260f, 0.300f, 0.360f, 1.000f}
#define ColorPrimSelectedBg {0.75, 0.60, 0.33, 0.6}
#define ColorAttributeSelectedBg {0.75, 0.60, 0.33, 0.6}

/// Predefined colors for dark mode ui components
#define ColorPrimaryLight {0.900f, 0.900f, 0.900f, 1.000f}

#define ColorSecondaryLight {0.490f, 0.490f, 0.490f, 1.000f}
#define ColorSecondaryLightLighten {0.586f, 0.586f, 0.586f, 1.000f}
#define ColorSecondaryLightDarken {0.400f, 0.400f, 0.400f, 1.000f}

#define ColorPrimaryDark {0.148f, 0.148f, 0.148f, 1.000f}
#define ColorPrimaryDarkLighten {0.195f, 0.195f, 0.195f, 1.000f}
#define ColorPrimaryDarkDarken {0.098f, 0.098f, 0.098f, 1.000f}

#define ColorSecondaryDark {0.340f, 0.340f, 0.340f, 1.000f}
#define ColorSecondaryDarkLighten {0.391f, 0.391f, 0.391f, 1.000f}
#define ColorSecondaryDarkDarken {0.280f, 0.280f, 0.280f, 1.000f}

#define ColorHighlight {0.880f, 0.620f, 0.170f, 1.000f}
#define ColorHighlightDarken {0.880f, 0.620f, 0.170f, 0.781f}

#define ColorBackgroundDim {0.000f, 0.000f, 0.000f, 0.586f}
#define ColorInvisible {0.000f, 0.000f, 0.000f, 0.000f}

/// Decimal Precision shown in the floating point values UI
constexpr const char * DecimalPrecision = "%.5f";

/// Default name when creating a prim
constexpr const char *const SdfPrimSpecDefaultName = "prim";

/// Return error codes
constexpr int ERROR_UNABLE_TO_COMPILE_SHADER = 110;

/// A set of icons for the application.
/// _UT_ stands for Usd Tweak
#define ICON_UT_DELETE ICON_FA_TRASH
#define ICON_UT_STAGE ICON_FA_DESKTOP
