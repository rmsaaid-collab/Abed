#pragma once
#include "il2cpp_types.h"

#define RVA_RustBridgeSimulatePool   0x04D26840UL
#define RVA_PoolGC_RotateCue         0x0518138CUL
#define RVA_PoolGC_ApplySpin         0x0518221CUL
#define RVA_PowerBarDragged          0x05181CACUL
#define RVA_PowerBarController_Awake 0x04821AECUL
#define RVA_PowerBar_CalculatePower  0x04821D0CUL
#define RVA_SetLastLocalShotInfo     0x05184BA0UL
#define RVA_BIH_LateUpdate           0x05176DE8UL
#define RVA_DropBallFromHand         0x051809F8UL
#define RVA_CueController_Shoot      0x0519EEE4UL
#define RVA_CueController_Update     0x0519E498UL
#define RVA_ShotInfoSerialize        0x0499BD48UL
#define RVA_FinishAnimating          0x05181E50UL
#define RVA_GameStore_get_GameID     0x047E162CUL
#define RVA_GameStore_Write          0x047E775CUL

#define RVA_GameStore_IsMyTurn       0x047D0DE0UL

#define RVA_OnTurnChanged            0x0517DE80UL

#define RVA_GameOver                 0x0518281CUL

#define RVA_TakeBallInHand           0x051806E0UL
#define RVA_MoveBallInHand           0x051807A4UL
#define RVA_MoveBallInHandInstantly  0x0518093CUL

#define RVA_get_QuickMatchArguments  0x05194268UL
#define RVA_set_QuickMatchArguments  0x051942B0UL

#define RVA_GM_get_Instance          0x04789414UL
#define RVA_GM_get_IsMyTurn          0x04788628UL
#define RVA_GM_get_IsOpen            0x04788534UL
#define RVA_GM_get_IsInHand          0x047886CCUL
#define RVA_GM_get_IsInAnimationState 0x047889DCUL
#define RVA_GM_get_IsAnimating       0x04788C84UL
#define RVA_GM_get_IsInTrickShots    0x047887C4UL
#define RVA_GM_get_IsConnected       0x04788E5CUL

#define OFF_GM_MyPlayerOwnsSolids    0x22
#define OFF_GM_GameOver              0x21   
#define OFF_GM_RemainingSolids       0x40   
#define OFF_GM_RemainingStripes      0x48   

#define OFF_GM_PowerMultiplier     0x54   
#define OFF_GM_InclineAngle        0x58   
#define OFF_GM_IsBallInHand        0x5C   
#define OFF_GM_BallInHandType      0x60   
#define OFF_GM_ShouldCallPocket      0x66
#define OFF_GM_CalledPocket          0x68
#define OFF_GM_CanHaveBallInHand     0x5D

#define RVA_PocketSelected           0x05181EA4UL

#define RVA_PGC_Shoot                0x051819B0UL

#define RVA_Camera_get_main                   0x09B34BC8UL
#define RVA_Screen_get_width                  0x09B42714UL
#define RVA_Screen_get_height                 0x09B4273CUL
#define RVA_get_worldToCameraMatrix_Injected  0x09B333C0UL
#define RVA_get_projectionMatrix_Injected     0x09B33570UL

#define RVA_GetCueSpin          0x0517F430UL
#define RVA_GetCuePower         0x0517F5D8UL
#define RVA_GetMaxMasseAngle    0x0517F780UL

#define GC_MAX_RETRIES_OFF  0x0AE21120UL

#define OFF_PGC_CueController       0x38
#define OFF_PGC_BallInHandCtrl      0x40
#define OFF_PGC_CalcRotCtrl         0x58
#define OFF_PGC_PowerBarCtrl        0x70
#define OFF_PGC_IsGameOver          0x170


#define OFF_BIH_activeFlag      0x84
#define OFF_BIH_isHandEnabled   0x85
#define OFF_BIH_cueBall_x       0x88
#define OFF_BIH_cueBall_z       0x90

#define OFF_CC_direction_x      0x64
#define OFF_CC_direction_z      0x6C
#define OFF_CC_spin_x           0x70
#define OFF_CC_spin_y           0x74

#define OFF_PBC_power           0x68

typedef float (*GetCueFloat_t)(void* instance, void* method);

typedef void* (*RustBridgeSim_t)(void* arr);
typedef void  (*GenericVoid_t)(void*);
typedef void* (*GetMainCamera_t)(void* method);
typedef int   (*Screen_getDim_t)(void* method);
typedef void  (*GetMatrixInjected_t)(void* cam, void* outMatrix);

typedef bool  (*GameStore_IsMyTurn_t)(void* method);
typedef void  (*OnTurnChanged_t)(void* instance, void* method);
typedef void  (*GameOver_t)(void* instance, void* callback, void* confetti, void* method);
typedef void  (*MoveBIHInstantly_t)(void* pgc, float x, float y, float z, void* method);
typedef void  (*PGC_Shoot_t)(void* pgc, float powerMultiplier, void* method);
typedef void* (*GM_getInstance_t)(void* method);
typedef void  (*PocketSelected_t)(void* pgc, int32_t pocket, void* method);
typedef void  (*TakeBIH_t)(void* pgc, void* method);

#define TABLE_WORLD_Y_DEFAULT 0.87f
#define BALL_RADIUS_WORLD   (0.0308f)

static const float POCKET_POS[6][2] = {
    {-1.27f, +0.635f},
    { 0.00f, +0.675f},
    {+1.27f, +0.635f},
    {-1.27f, -0.635f},
    { 0.00f, -0.675f},
    {+1.27f, -0.635f},
};
