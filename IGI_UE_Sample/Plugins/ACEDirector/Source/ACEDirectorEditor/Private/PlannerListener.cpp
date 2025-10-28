#include "PlannerListener.h"
#include "CommandRouterComponent.h"

void UPlannerListener::Init(UCommandRouterComponent* InRouter)
{
    Router = InRouter;
    if (Router)
    {
        // Bind to your existing multicast delegate
        Router->OnPlannerText.AddDynamic(this, &UPlannerListener::HandlePlannerText);
    }
}

void UPlannerListener::HandlePlannerText(const FString& VisibleText)
{
    OnPlannerTextNative.Broadcast(VisibleText);
}
