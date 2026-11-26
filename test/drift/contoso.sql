-- Queries over the contoso view for the drift check: the extension must rewrite each one to the
-- SQL dax-sql gives in Node. One query a paragraph.

SELECT Category.Category, Product.Color, AGG("Sales Amount") AS amount, AGG("Share of Category") AS share
FROM contoso GROUP BY ALL ORDER BY Category, Color;

SELECT "Date".Year, "Date".Month, AGG("Sales YTD") AS ytd, AGG("Sales PY") AS py FROM contoso GROUP BY ALL ORDER BY Year, Month;

SELECT "Time Calc"."Time Calc", AGG("Sales Amount") AS amount FROM contoso GROUP BY ALL ORDER BY 1;

SELECT Customer.City, AGG(Customers) AS customers, AGG("Target Total") AS target FROM contoso
WHERE Product.Color IN ('Red', 'Blue') GROUP BY ALL HAVING AGG(Orders) > 0 ORDER BY City;

SELECT Category.Category, Product.Color, AGG(Quantity) AS q, GROUPING(Product.Color) AS subtotal
FROM contoso GROUP BY Category.Category, ROLLUP(Product.Color) ORDER BY 1, 2;

WITH m AS (SELECT Product.ProductKey, AGG("Sales Amount") AS amount FROM contoso GROUP BY ALL)
SELECT p."Name", m.amount FROM m JOIN "Product" p USING ("ProductKey") ORDER BY amount DESC;

CREATE TABLE top_colors AS SELECT Color, AGG(Orders) AS orders FROM contoso GROUP BY ALL;

EXPLAIN SELECT Color, AGG("Big Orders") AS big FROM contoso GROUP BY ALL;
