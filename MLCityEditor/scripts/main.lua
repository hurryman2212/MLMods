-- UI companion to the MLCityEditor native artisan cancellation handler.
-- The game omits extension slot zero once artisanJob is nonzero.
local inventory_class = "/Game/ManorLords/UI/Widgets/Widgets_Inventory/Inventory.Inventory_C"
local extension_class = "/Game/UI/Inventory/extensionSlotWidget.extensionSlotWidget_C"
local recipe_class = "/Game/UI/Inventory/W_InventoryTab_Recipes.W_InventoryTab_Recipes_C"
local visible, collapsed = 0, 1
local inventory_hook, extension_hook = false, false
local recipe_init_hook, recipe_update_hook = false, false
local inventory_slots, custom_slots, reported_errors = {}, {}, {}
local slot_inventories, pending_retries = {}, {}
local recipe_states, recipe_refreshing = {}, {}
local retry_delay_ms, maximum_retry_attempts = 250, 120

local function valid(object)
    return object ~= nil and object:IsValid()
end

local function report_error(operation, message)
    if not reported_errors[operation] then
        reported_errors[operation] = true
        print("[MLCityEditor] Extension cancel UI " .. operation .. ": " .. tostring(message) .. "\n")
    end
end

local function eligible(building)
    if not valid(building) or type(MLCityEditorCanCancelArtisan) ~= "function"
        or MLCityEditorCanCancelArtisan(building:GetAddress()) ~= true then
        return false
    end
    -- Preserve the native small-holding exclusion.
    local upgrades = building.upgradesDone
    for index = 1, #upgrades do
        if upgrades[index] == 24 then
            return false
        end
    end
    return true
end

local function update_cancel_slot(widget)
    if not valid(widget)
        or custom_slots[widget:GetAddress()] ~= widget:GetFullName() then
        return
    end
    local can_cancel = eligible(widget.building)
    widget:SetIsEnabled(can_cancel)
    widget.DemolishButton:SetVisibility(can_cancel and visible or collapsed)
    widget.DemolishButton:SetIsEnabled(can_cancel)
    -- This property alone is unused by the current Blueprint. Hide the actual
    -- options container as well, so the restored slot only offers cancellation.
    widget.showConversionOptions = false
    widget.ExtensionGridNamedSlot:SetVisibility(collapsed)
    widget.ExtensionGridNamedSlot:SetIsEnabled(false)
    if valid(widget.SlotGrid) then
        widget.SlotGrid:SetVisibility(collapsed)
        widget.SlotGrid:SetIsEnabled(false)
    end
    widget.exclam:SetVisibility(collapsed)
    local tooltip = FText("Cancel backyard workshop")
    widget.main_button:SetToolTip(nil)
    widget.main_button:SetToolTipText(tooltip)
    widget.DemolishButton:SetToolTipText(tooltip)
    return can_cancel
end

local function append_cancel_slot(inventory)
    if not valid(inventory) or not valid(inventory.Building) then
        return false
    end
    local container = inventory.extensionSlots_HB
    if not valid(container) then
        return false
    end
    local can_cancel = eligible(inventory.Building)
    -- A notification can arrive after the first update already ran. Also avoid
    -- duplicates when another UI refresh calls our post hook more than once.
    for index = 0, container:GetChildrenCount() - 1 do
        local child = container:GetChildAt(index)
        if valid(child) and child:IsA(extension_class) and child.id == 0 then
            if custom_slots[child:GetAddress()] == child:GetFullName() then
                if not valid(child.building)
                    or child.building:GetAddress() ~= inventory.Building:GetAddress()
                    or child.building:GetFullName() ~= inventory.Building:GetFullName() then
                    return false
                end
                return update_cancel_slot(child)
            end
            return true
        end
    end
    if not can_cancel then
        return false
    end
    local widget_class = StaticFindObject(extension_class)
    local library = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    if not valid(widget_class) or not valid(library) then
        return false
    end
    local widget = library:Create(inventory, widget_class, nil)
    if not valid(widget) then
        return false
    end
    widget.building = inventory.Building
    widget.id = 0
    widget.ExtensionIndex = 0
    -- UE4SS copies the struct with FStructProperty::CopyCompleteValue. This is
    -- the widget's own copy; no building or extension state is changed here.
    widget.extension = inventory.Building.extensions[1]
    widget.showConversionOptions = false
    local inventory_address = inventory:GetAddress()
    local previous = inventory_slots[inventory_address]
    if previous and custom_slots[previous.address] == previous.name then
        custom_slots[previous.address] = nil
        slot_inventories[previous.address] = nil
    end
    local identity = {address = widget:GetAddress(), name = widget:GetFullName()}
    inventory_slots[inventory_address] = identity
    custom_slots[identity.address] = identity.name
    slot_inventories[identity.address] = inventory
    local slot = container:AddChildToHorizontalBox(widget)
    if valid(slot) then
        slot:SetPadding({Left = 2, Top = 2, Right = 2, Bottom = 2})
    end
    widget:SetIsEnabled(true)
    widget:updateContents()
    update_cancel_slot(widget)
    -- The unmodified main button opens the native options panel. Its existing
    -- DemolishButton queues addExtensionChangeRequest(building, 0, 0).
    return true
end

local function refresh_cancel_slot(inventory)
    if not valid(inventory) then return end
    local inventory_address = inventory:GetAddress()
    if append_cancel_slot(inventory) then
        pending_retries[inventory_address] = nil
        return
    end
    local building = inventory.Building
    if not valid(building) or type(MLCityEditorCanCancelArtisan) ~= "function"
        or building.artisanJob <= 0 or building.buildingFunction ~= 1
        or #building.extensions == 0 then
        pending_retries[inventory_address] = nil
        return
    end
    local owner = building.ownerPawn
    if not valid(owner) or not owner.isMainPlayer or owner.isAI then return end
    for index = 1, #building.upgradesDone do
        if building.upgradesDone[index] == 24 then return end
    end
    local inventory_name = inventory:GetFullName()
    local building_address, building_name = building:GetAddress(), building:GetFullName()
    local existing = pending_retries[inventory_address]
    if existing and existing.inventory_name == inventory_name
        and existing.building_address == building_address and existing.building_name == building_name then
        return
    end
    local retry = {inventory_name = inventory_name, building_address = building_address,
        building_name = building_name, remaining = maximum_retry_attempts}
    pending_retries[inventory_address] = retry
    local function retry_later()
        ExecuteWithDelay(retry_delay_ms, function()
            -- The delay callback is asynchronous; touch Unreal only after
            -- switching back to the game thread and rechecking both identities.
            local callback = function()
                if pending_retries[inventory_address] ~= retry then return end
                local ok, message = pcall(function()
                    if not valid(inventory) or inventory:GetFullName() ~= inventory_name
                        or inventory.closing or not inventory:IsVisible() then
                        pending_retries[inventory_address] = nil
                        return
                    end
                    local current = inventory.Building
                    if not valid(current) or current:GetAddress() ~= building_address
                        or current:GetFullName() ~= building_name or current.artisanJob <= 0 then
                        pending_retries[inventory_address] = nil
                        return
                    end
                    if append_cancel_slot(inventory) then
                        pending_retries[inventory_address] = nil
                        return
                    end
                    retry.remaining = retry.remaining - 1
                    if retry.remaining > 0 then
                        retry_later()
                    else
                        pending_retries[inventory_address] = nil
                        local native_ready = type(MLCityEditorCanCancelArtisan) == "function"
                            and MLCityEditorCanCancelArtisan(current:GetAddress()) == true
                        local reason = native_ready and "native query=true; UI class/container unavailable"
                            or "native query=false; cancellation remains blocked"
                        report_error("retry expired " .. building_name, reason)
                    end
                end)
                if not ok then
                    pending_retries[inventory_address] = nil
                    report_error("deferred refresh", message)
                end
            end
            local ok, message = pcall(function()
                if EngineTickAvailable and type(EGameThreadMethod) == "table"
                    and EGameThreadMethod.EngineTick ~= nil then
                    ExecuteInGameThread(callback, EGameThreadMethod.EngineTick)
                else
                    ExecuteInGameThread(callback)
                end
            end)
            if not ok then
                pending_retries[inventory_address] = nil
                report_error("deferred scheduling", message)
            end
        end)
    end
    retry_later()
end

local function refresh_recipe_choices(tab, initialized)
    if not valid(tab) or type(MLCityEditorCanCancelArtisan) ~= "function" then
        return
    end
    local address = tab:GetAddress()
    if recipe_refreshing[address] then return end
    local building, focus = tab.Building, tab.productionFocusWidget
    if not valid(building) or not valid(focus) or building.buildingFunction ~= 1 then
        recipe_states[address] = nil
        return
    end
    local owner = building.ownerPawn
    if not valid(owner) or not owner.isMainPlayer or owner.isAI then
        recipe_states[address] = nil
        return
    end
    local state = {tab_name = tab:GetFullName(), focus_address = focus:GetAddress(),
        focus_name = focus:GetFullName(), building_address = building:GetAddress(),
        building_name = building:GetFullName(), artisan = building.artisanJob}
    local previous = recipe_states[address]
    -- In 0.8.117 BP_OnInit performs the native population once, while BP_Update
    -- only updates highlights. Reuse that native population on a workshop or
    -- building change; it clears the old HB children before reading current
    -- GetAllProducedGoods/GetActiveRecipes. Native tab visibility is untouched.
    if initialized or (previous and previous.tab_name == state.tab_name
        and previous.focus_address == state.focus_address and previous.focus_name == state.focus_name
        and previous.building_address == state.building_address
        and previous.building_name == state.building_name and previous.artisan == state.artisan) then
        recipe_states[address] = state
        return
    end
    recipe_refreshing[address] = true
    local ok, message = pcall(function()
        -- This is the same widget-local assignment as native BP_OnInit.
        focus.building = building
        focus["Repopulate Choices"](focus)
        focus:updateChoiceSelectionHighlight()
    end)
    recipe_refreshing[address] = nil
    if not ok then error(message) end
    recipe_states[address] = state
end

local function install_hooks()
    -- Blueprint functions use the second RegisterHook callback as a post hook.
    -- The optional third callback only applies to native /Script functions.
    if not extension_hook and valid(StaticFindObject(extension_class .. ":updateContents")) then
        RegisterHook(extension_class .. ":updateContents", function(context)
            local ok, message = pcall(function()
                local widget = context:get()
                if update_cancel_slot(widget) == false then
                    local inventory = slot_inventories[widget:GetAddress()]
                    if valid(inventory) then refresh_cancel_slot(inventory) end
                end
            end)
            if not ok then report_error("refresh", message) end
        end)
        extension_hook = true
    end
    if not inventory_hook and extension_hook
        and valid(StaticFindObject(inventory_class .. ":updateExtensionSlots")) then
        RegisterHook(inventory_class .. ":updateExtensionSlots", function(context)
            local ok, message = pcall(refresh_cancel_slot, context:get())
            if not ok then report_error("append", message) end
        end)
        inventory_hook = true
        print("[MLCityEditor] Native artisan extension cancel UI hooks registered.\n")
    end
    if not recipe_init_hook and valid(StaticFindObject(recipe_class .. ":BP_OnInit")) then
        RegisterHook(recipe_class .. ":BP_OnInit", function(context)
            local ok, message = pcall(refresh_recipe_choices, context:get(), true)
            if not ok then report_error("recipe initialization", message) end
        end)
        recipe_init_hook = true
    end
    if not recipe_update_hook and valid(StaticFindObject(recipe_class .. ":BP_Update")) then
        RegisterHook(recipe_class .. ":BP_Update", function(context)
            local ok, message = pcall(refresh_recipe_choices, context:get(), false)
            if not ok then report_error("recipe refresh", message) end
        end)
        recipe_update_hook = true
    end
end

NotifyOnNewObject(inventory_class, function(inventory)
    local ok, message = pcall(function()
        install_hooks()
        if inventory_hook then refresh_cancel_slot(inventory) end
    end)
    if not ok then report_error("inventory initialization", message) end
end)

NotifyOnNewObject(extension_class, function()
    local ok, message = pcall(install_hooks)
    if not ok then report_error("extension initialization", message) end
    return inventory_hook and extension_hook and recipe_init_hook and recipe_update_hook
end)

NotifyOnNewObject(recipe_class, function()
    local ok, message = pcall(install_hooks)
    if not ok then report_error("recipe tab initialization", message) end
    return recipe_init_hook and recipe_update_hook
end)

local ok, message = pcall(install_hooks)
if not ok then report_error("registration", message) end
