local m = {}
for i=0,199999 do m["k"..i] = i end
local total = 0
for i=0,199999 do total = total + m["k"..i] end
print(total)
