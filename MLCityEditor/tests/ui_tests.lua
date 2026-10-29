local script_path = assert(arg[1], "usage: lua ui_tests.lua scripts/main.lua")
local checks = 0
local function expect(value, message)
    checks = checks + 1
    assert(value, message)
end

local next_address, hooks, notifications = 1000, {}, {}
local function object(name)
    next_address = next_address + 1
    local result = {address = next_address, name = name, alive = true}
    function result:IsValid() return self.alive end
    function result:GetAddress() return self.address end
    function result:GetFullName() return self.name end
    return result
end

function StaticFindObject(name) return object(name) end
function RegisterHook(name, callback)
    expect(hooks[name] == nil, "a hook is installed only once")
    hooks[name] = callback
end
function NotifyOnNewObject(name, callback) notifications[name] = callback end
function MLCityEditorCanCancelArtisan() return true end

assert(loadfile(script_path))()
local recipe_class = "/Game/UI/Inventory/W_InventoryTab_Recipes.W_InventoryTab_Recipes_C"
local function dispatch(event, tab)
    local callback = assert(hooks[recipe_class .. ":" .. event], "recipe hook registered")
    callback({get = function() return tab end})
end
expect(hooks[recipe_class .. ":BP_OnInit"] ~= nil, "current recipe initializer is hooked")
expect(hooks[recipe_class .. ":BP_Update"] ~= nil, "current recipe updater is hooked")
for name in pairs(hooks) do
    expect(not name:find("Update Production Focus Widget if Needed", 1, true), "obsolete function is not hooked")
end

local owner = object("MainPlayer")
owner.isMainPlayer, owner.isAI = true, false
local building = object("HouseA")
building.ownerPawn, building.buildingFunction, building.artisanJob = owner, 1, 74
building.inventory_marker = "unchanged"
local tab = object("RecipeTabA")
tab.Building, tab.visibility = building, "native-owned"
local populations, highlights, inject_error, inject_recursion = 0, 0, false, false
local function focus_widget(name)
    local focus = object(name)
    focus.building, focus.choices = building, {}
    focus["Repopulate Choices"] = function(self)
        if inject_error then error("simulated native UI failure") end
        populations = populations + 1
        self.choices = {}
        if self.building.artisanJob == 74 then self.choices = {"Cheese"} end
        if self.building.artisanJob == 22 then self.choices = {"Ale"} end
        if inject_recursion then dispatch("BP_Update", tab) end
    end
    function focus:updateChoiceSelectionHighlight() highlights = highlights + 1 end
    return focus
end
tab.productionFocusWidget = focus_widget("FocusA")
local focus = tab.productionFocusWidget

-- The game performs this population before our BP_OnInit post hook.
focus["Repopulate Choices"](focus)
dispatch("BP_OnInit", tab)
expect(populations == 1, "initializer does not duplicate native population")
dispatch("BP_Update", tab)
expect(populations == 1, "unchanged updates do not rebuild choices")
building.artisanJob = 0
dispatch("BP_Update", tab)
expect(populations == 2 and #focus.choices == 0, "cancellation clears old choices through native population")
building.artisanJob = 22
dispatch("BP_Update", tab)
expect(populations == 3 and focus.choices[1] == "Ale", "cheese to brewery switches output choices")
dispatch("BP_Update", tab)
expect(populations == 3, "stable brewery does not repopulate each update")
-- If cancellation and rebuilding the same profession happen between updates,
-- the output types are unchanged. The native BP_Update still owns highlights.
building.artisanJob = 0
building.artisanJob = 22
dispatch("BP_Update", tab)
expect(populations == 3 and focus.choices[1] == "Ale", "unobserved cancellation followed by the same artisan retains the correct output types")
building.artisanJob = 0
dispatch("BP_Update", tab)
building.artisanJob = 74
dispatch("BP_Update", tab)
expect(populations == 5 and focus.choices[1] == "Cheese", "repeat cancellation and rebuilding remain functional")
expect(tab.visibility == "native-owned", "native tab visibility is never overridden")
expect(building.inventory_marker == "unchanged", "gameplay data is untouched")

local replacement = object("HouseB")
replacement.ownerPawn, replacement.buildingFunction, replacement.artisanJob = owner, 1, 74
tab.Building = replacement
dispatch("BP_Update", tab)
expect(populations == 6 and focus.building == replacement, "new building identity updates the widget-local reference")
replacement.name = "HouseBRecycled"
dispatch("BP_Update", tab)
expect(populations == 7, "reused building address with a different name is detected")
tab.name = "RecipeTabRecycled"
dispatch("BP_Update", tab)
expect(populations == 8, "reused tab identity cannot retain old recipe state")
focus.name = "FocusRecycled"
dispatch("BP_Update", tab)
expect(populations == 9, "reused focus identity triggers fresh native choices")

inject_error = true
replacement.artisanJob = 22
dispatch("BP_Update", tab)
expect(populations == 9, "failed population does not publish new state")
inject_error = false
dispatch("BP_Update", tab)
expect(populations == 10 and focus.choices[1] == "Ale", "failure clears the reentrancy guard and retries safely")
inject_recursion = true
replacement.artisanJob = 74
dispatch("BP_Update", tab)
inject_recursion = false
expect(populations == 11, "recursive UI updates do not recurse into population")

owner.isAI = true
replacement.artisanJob = 22
dispatch("BP_Update", tab)
expect(populations == 11, "AI buildings are untouched")
owner.isAI = false
dispatch("BP_Update", tab)
expect(populations == 12, "return to a valid player context refreshes choices")
replacement.buildingFunction = 2
dispatch("BP_Update", tab)
expect(populations == 12, "nonresidential buildings are untouched")
replacement.buildingFunction = 1
focus.alive = false
dispatch("BP_Update", tab)
expect(populations == 12, "invalid child widgets are not called")
tab.productionFocusWidget = focus_widget("ReplacementFocus")
dispatch("BP_Update", tab)
expect(populations == 13 and tab.productionFocusWidget.building == replacement, "replacement child receives the current building")
MLCityEditorCanCancelArtisan = nil
replacement.artisanJob = 74
dispatch("BP_Update", tab)
expect(populations == 13, "missing native bridge disables the companion action")
expect(highlights == populations - 1, "each successful repair also runs native highlighting")
print("City UI tests passed: " .. checks)
