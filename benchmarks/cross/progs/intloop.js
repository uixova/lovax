function compute(n) {
  let acc = 0, i = 0;
  while (i < n) {
    acc = acc + (i * 3 + 7) % 13;
    acc = acc % 30011;
    i = i + 1;
  }
  return acc;
}
console.log(compute(30000000));
