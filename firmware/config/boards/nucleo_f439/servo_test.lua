	testPos = {0, 25, 50, 75, 100, 0}
	idx = 1
	thr_timer = Timer.new()
	thr_timer:reset()
	hold = 2.0
	testActive = false
	prevTarget = 0
	tests = 0
	passed = 0

	autoTest = 0
	manualTarget = 0

	function onTick()
		local autoTestEnable = autoTest
		local manual = manualTarget
		
		if autoTestEnable > 0 then
			testActive = true
		else
			testActive = false
		end
		
		if testActive then
			if thr_timer:getElapsedSeconds() >= hold then
				thr_timer:reset()
				local tps = getSensor("Tps1")
				if tests > 0 and tps then
					local err = math.abs(tps - prevTarget)
					if err <= 3.0 then
						print("PASS: Target=" .. prevTarget .. " TPS=" .. tps)
						passed = passed + 1
					else
						print("FAIL: Target=" .. prevTarget .. " TPS=" .. tps .. " Error=" .. err)
					end
				end
				local target = testPos[idx]
				prevTarget = target
				print("Step " .. idx .. "/" .. #testPos .. ": " .. target)
				print("Console: set accel_pedal " .. target)
				idx = idx + 1
				tests = tests + 1
				if idx > #testPos then
					idx = 1
					print("CYCLE DONE: " .. passed .. "/" .. tests .. " passed")
					tests = 0
					passed = 0
				end
			end
		else
			if manual ~= prevTarget then
				prevTarget = manual
				print("Manual: " .. manual)
				print("Console: set accel_pedal " .. manual)
			end
			if thr_timer:getElapsedSeconds() >= 1.0 then
				thr_timer:reset()
				local tps = getSensor("Tps1")
				if tps then
					print("TPS=" .. tps .. " Target=" .. manual)
				end
			end
		end
	end

	print("Servo Test Ready")
	print("lua autoTest=1 for auto test")
	print("lua manualTarget=X for manual")
