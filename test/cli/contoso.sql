-- The extension in the DuckDB CLI: dax-sql's contoso model over its data, created, queried and
-- inspected. Run by CI's cli job after LOAD; -bail stops at the first error.
CREATE TABLE "Sales" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Sales.csv', header = true, types = {'Amount': 'DECIMAL(18,4)'});
CREATE TABLE "Product" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Product.csv', header = true);
CREATE TABLE "Category" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Category.csv', header = true);
CREATE TABLE "Customer" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Customer.csv', header = true);
CREATE TABLE "Date" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Date.csv', header = true);
CREATE TABLE "Returns" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Returns.csv', header = true);
CREATE TABLE "CityTarget" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/CityTarget.csv', header = true);
CREATE TABLE "Staff" AS SELECT * FROM read_csv('third_party/dax-sql/conformance/models/contoso/data/Staff.csv', header = true);

CREATE SEMANTIC VIEW contoso FROM 'third_party/dax-sql/conformance/models/contoso/model.bim';
SHOW SEMANTIC VIEWS;

SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount FROM contoso GROUP BY ALL ORDER BY Color;

WITH m AS (SELECT Category.Category, AGG(Quantity) AS q FROM contoso GROUP BY ALL)
SELECT m.*, c."CategoryKey" FROM m JOIN "Category" c USING ("Category") ORDER BY ALL;

DESCRIBE contoso;
SHOW SEMANTIC METRICS IN contoso;
SHOW SEMANTIC DIMENSIONS IN contoso FOR METRIC Orders;
