pos = {0, 25, 50, 75, 100, 0}
i = 1
t = Timer.new()
t:reset()
active = false
prev = 0
cnt = 0
ok = 0

auto = 0
manual = 0

function onTick()
if auto > 0 then
active = true
else
active = false
end

if active then
if t:getElapsedSeconds() >= 2 then
t:reset()
local tps = getSensor("Tps1")
if cnt > 0 and tps then
local e = math.abs(tps - prev)
if e <= 3 then
print("PASS T=" .. prev .. " TPS=" .. tps)
ok = ok + 1
else
print("FAIL T=" .. prev .. " TPS=" .. tps .. " E=" .. e)
end
end
local target = pos[i]
prev = target
print("Step " .. i .. "/" .. #pos .. ": " .. target)
print("Run: set accel_pedal " .. target)
i = i + 1
cnt = cnt + 1
if i > #pos then
i = 1
print("Done: " .. ok .. "/" .. cnt .. " passed")
cnt = 0
ok = 0
end
end
else
if manual ~= prev then
prev = manual
print("Manual: " .. manual)
end
if t:getElapsedSeconds() >= 1 then
t:reset()
local tps = getSensor("Tps1")
if tps then
print("TPS=" .. tps .. " Target=" .. manual)
end
end
end
end

print("Servo Test v2")
print("lua auto=1 for test")
print("lua manual=X for manual")
