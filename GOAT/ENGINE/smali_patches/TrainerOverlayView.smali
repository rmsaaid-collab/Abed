.class public Lcom/google/firebase/TrainerOverlayView;
.super Landroid/view/View;
.implements Landroid/view/View$OnClickListener;
.implements Landroid/widget/SeekBar$OnSeekBarChangeListener;
.implements Landroid/view/View$OnTouchListener;

# Static field: cached screen coords from native
.field public static cachedCoords:[F

# Constructor
.method public constructor <init>(Landroid/content/Context;)V
    .registers 3
    invoke-direct {p0, p1}, Landroid/view/View;-><init>(Landroid/content/Context;)V
    const/4 v0, 0x0
    invoke-virtual {p0, v0}, Landroid/view/View;->setWillNotDraw(Z)V
    return-void
.end method

# Native methods
.method public static native nativeOnDraw(Landroid/view/View;Landroid/graphics/Canvas;[F)V
.end method

.method public static native nativeOnClick(I)V
.end method

.method public static native nativeOnTouch(IFFI)Z
.end method

.method public static native nativeOnForceChanged(I)V
.end method

.method public static native nativeCreatePanel(Landroid/app/Activity;Lcom/google/firebase/TrainerOverlayView;)V
.end method

# OnClickListener
.method public onClick(Landroid/view/View;)V
    .registers 4
    invoke-virtual {p1}, Landroid/view/View;->getId()I
    move-result v0
    invoke-static {v0}, Lcom/google/firebase/TrainerOverlayView;->nativeOnClick(I)V
    return-void
.end method

# OnTouchListener
.method public onTouch(Landroid/view/View;Landroid/view/MotionEvent;)Z
    .registers 7
    # v0=viewId, v1=rawX, v2=rawY, v3=action
    invoke-virtual {p1}, Landroid/view/View;->getId()I
    move-result v0
    invoke-virtual {p2}, Landroid/view/MotionEvent;->getRawX()F
    move-result v1
    invoke-virtual {p2}, Landroid/view/MotionEvent;->getRawY()F
    move-result v2
    invoke-virtual {p2}, Landroid/view/MotionEvent;->getAction()I
    move-result v3
    invoke-static {v0, v1, v2, v3}, Lcom/google/firebase/TrainerOverlayView;->nativeOnTouch(IFFI)Z
    move-result v0
    return v0
.end method

# OnSeekBarChangeListener
.method public onProgressChanged(Landroid/widget/SeekBar;IZ)V
    .registers 5
    invoke-static {p2}, Lcom/google/firebase/TrainerOverlayView;->nativeOnForceChanged(I)V
    return-void
.end method

.method public onStartTrackingTouch(Landroid/widget/SeekBar;)V
    .registers 2
    return-void
.end method

.method public onStopTrackingTouch(Landroid/widget/SeekBar;)V
    .registers 2
    return-void
.end method

# onDraw override
.method protected onDraw(Landroid/graphics/Canvas;)V
    .registers 4
    sget-object v0, Lcom/google/firebase/TrainerOverlayView;->cachedCoords:[F
    if-nez v0, :has_data
    const/4 v1, 0x0
    new-array v0, v1, [F
    :has_data
    invoke-static {p0, p1, v0}, Lcom/google/firebase/TrainerOverlayView;->nativeOnDraw(Landroid/view/View;Landroid/graphics/Canvas;[F)V
    return-void
.end method
