local total = 0.0
for i = 0, 3999999 do
  local dx = i * 0.001; local dy = i * 0.0005
  total = total + math.sqrt(dx*dx + dy*dy)
end
print(total)
