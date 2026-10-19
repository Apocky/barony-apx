-- The hello script: proves the layer loads and the first event fires. Safe to ship; says one line per floor.
apx.on("level_entered", function(ev)
  apx.log(string.format("hello: floor %d (%s), seed %d, %d monsters, %dx%d",
    ev.floor, ev.name, ev.seed, ev.monsters, ev.width, ev.height))
end)
