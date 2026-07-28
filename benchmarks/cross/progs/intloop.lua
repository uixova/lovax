local function compute(n)
  local acc = 0
  local i = 0
  while i < n do
    acc = acc + (i * 3 + 7) % 13
    acc = acc % 30011
    i = i + 1
  end
  return acc
end
print(compute(30000000))
