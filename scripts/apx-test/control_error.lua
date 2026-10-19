-- CONTROL: three kinds of script failure. The layer must catch and log each; the game must keep running.
apx.on("level_entered", function() error("control: deliberate error in a handler") end)
apx.on("level_entered", function() local n = 0; while true do n = n + 1 end end) -- instruction budget
apx.after(20, function() local t = nil; return t.x end)                         -- runtime error in a timer
