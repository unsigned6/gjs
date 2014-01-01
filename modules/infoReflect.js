/*
 * Copyright (c) 2014 Endless Mobile, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authored By: Sam Spilsbury <sam@endlessm.com>
 */

function getSubNodesForNode(node) {
    let subNodes = [];
    switch (node.type) {
    /* These statements have a single body */
    case 'LabelledStatement':
    case 'WithStatement':
    case 'LetStatement':
    case 'ForInStatement':
    case 'ForOfStatement':
    case 'FunctionDeclaration':
    case 'FunctionExpression':
    case 'ArrowExpression':
    case 'CatchClause':
        subNodes.push(node.body);
        break;
    case 'WhileStatement':
    case 'DoWhileStatement':
        subNodes.push(node.body);
        subNodes.push(node.test);
        break;
    case 'ForStatement':
        if (node.init !== null)
            subNodes.push(node.init);
        if (node.test !== null)
            subNodes.push(node.test);
        if (node.update !== null)
            subNodes.push(node.update);

        subNodes.push(node.body);
        break;
    case 'BlockStatement':
        Array.prototype.push.apply(subNodes, node.body);
        break;
    case 'ThrowStatement':
    case 'ReturnStatement':
        if (node.argument !== null)
            subNodes.push(node.argument);
        break;
    case 'ExpressionStatement':
        subNodes.push(node.expression);
        break;
    case 'AssignmentExpression':
        subNodes.push(node.left, node.right);
        break;
    case 'ObjectExpression':
        node.properties.forEach(function(prop) {
            subNodes.push(prop.value);
        });
        break;
    /* It is very possible that there might be something
     * interesting in the function arguments, so we need to
     * walk them too */
    case 'NewExpression':
    case 'CallExpression':
        Array.prototype.push.apply(subNodes, node.arguments);
        subNodes.push(node.callee);
        break;
    /* These statements might have multiple different bodies
     * depending on whether or not they were entered */
    case 'IfStatement':
        subNodes = [node.test, node.consequent];
        if (node.alternate !== null)
            subNodes.push(node.alternate);
        break;
    case 'TryStatement':
        subNodes = [node.block];
        if (node.handler !== null)
            subNodes.push(node.handler);
        if (node.finalizer !== null)
            subNodes.push(node.finalizer);
        break;
    case 'SwitchStatement':
        for (let caseClause of node.cases) {
            caseClause.consequent.forEach(function(expression) {
                subNodes.push(expression);
            });
        }

        break;
    /* Variable declarations might be initialized to
     * some expression, so traverse the tree and see if
     * we can get into the expression */
    case 'VariableDeclaration':
        node.declarations.forEach(function (declarator) {
            if (declarator.init !== null)
                subNodes.push(declarator.init);
        });

        break;
    }

    return subNodes;
}

function collectForSubNodes(subNodes, collector) {
    let result = [];
    if (subNodes !== undefined &&
        subNodes.length > 0) {

        subNodes.forEach(function(node) {
            let subResult = collector(node);
            if (subResult !== undefined)
                Array.prototype.push.apply(result, subResult);

            Array.prototype.push.apply(result,
                                       collectForSubNodes(getSubNodesForNode(node),
                                                          collector));
        });
    }

    return result;
}

/* Unfortunately, the Reflect API doesn't give us enough information to
 * uniquely identify a function. A function might be anonymous, in which
 * case the JS engine uses some heurisitics to get a unique string identifier
 * but that isn't available to us here.
 *
 * There's also the edge-case where functions with the same name might be
 * defined within the same scope, or multiple anonymous functions might
 * be defined on the same line. In that case, it will look like we entered
 * the same function multiple times since we can't get column information
 * from the engine-side.
 *
 * For instance:
 *
 * 1. function f() {
 *       function f() {
 *       }
 *    }
 *
 * 2. let a = function() { function(a, b) {} };
 *
 * 3. let a = function() { function () {} }
 *
 * We can work-around case 1 by using the line numbers to get a unique identifier.
 * We can work-around case 2 by using the arguments length to get a unique identifier
 * We can't work-around case 3. The best thing we can do is warn that coverage
 * reports might be inaccurate as a result */
function functionsForNode(node) {
    let functionNames = [];
    switch (node.type) {
    case 'FunctionDeclaration':
    case 'FunctionExpression':
        if (node.id !== null) {
            functionNames.push({ name: node.id.name,
                                 line: node.loc.start.line,
                                 n_params: node.params.length });
        }
        /* If the function wasn't found, we just push a name
         * that looks like 'function:lineno' to signify that
         * this was an anonymous function. If the coverage tool
         * enters a function with no name (but a line number)
         * then it can probably use this information to
         * figure out which function it was */
        else {
            functionNames.push({ name: null,
                                 line: node.loc.start.line,
                                 n_params: node.params.length });
        }
    }

    return functionNames;
}

function functionsForAST(ast) {
    return collectForSubNodes(ast.body, functionsForNode);
}

/* If a branch' consequent is a block statement, there's
 * a chance that it could start on the same line, although
 * that's not where execution really starts. If it is
 * a block statement then handle the case and go
 * to the first line where execution starts */
function getBranchExitStartLine(branchBodyNode) {
    switch (branchBodyNode.type) {
    case 'BlockStatement':
        /* Hit a block statement, but nothing inside, can never
         * be executed, tell the upper level to move on to the next
         * statement */
        if (branchBodyNode.body.length === 0)
            return -1;

        /* Handle the case where we have nested block statements
         * that never actually get to executable code by handling
         * all statements within a block */
        for (let statement of branchBodyNode.body) {
            let startLine = getBranchExitStartLine(statement);
            if (startLine !== -1)
                return startLine;
        }

        /* Couldn't find an executable line inside this block */
        return -1;

    case 'SwitchCase':
        /* Hit a switch, but nothing inside, can never
         * be executed, tell the upper level to move on to the next
         * statement */
        if (branchBodyNode.consequent.length === 0)
            return -1;

        /* Handle the case where we have nested block statements
         * that never actually get to executable code by handling
         * all statements within a block */
        for (let statement of branchBodyNode.consequent) {
            let startLine = getBranchExitStartLine(statement);
            if (startLine !== -1) {
                return startLine;
            }
        }

        /* Couldn't find an executable line inside this block */
        return -1;
    /* These types of statements are never executable */
    case 'EmptyStatement':
    case 'LabelledStatement':
        return -1;
    default:
        break;
    }

    return branchBodyNode.loc.start.line;
}

function branchesForNode(node) {
    let branches = [];

    let branchExitNodes = [];
    switch (node.type) {
    case 'IfStatement':
        branchExitNodes.push(node.consequent);
        if (node.alternate !== null)
            branchExitNodes.push(node.alternate);
        break;
    case 'WhileStatement':
    case 'DoWhileStatement':
        branchExitNodes.push(node.body);
        break;
    case 'SwitchStatement':

        /* The case clauses by themselves are never executable
         * so find the actual exits */
        Array.prototype.push.apply(branchExitNodes, node.cases);
        break;
    default:
        break;
    }

    let branchExitStartLines = branchExitNodes.map(getBranchExitStartLine);
    branchExitStartLines = branchExitStartLines.filter(function(line) {
        return line !== -1;
    });

    /* Branch must have at least one exit */
    if (branchExitStartLines.length) {
        branches.push({ point: node.loc.start.line,
                        exits: branchExitStartLines });
    }

    return branches;
}

function branchesForAST(ast) {
    return collectForSubNodes(ast.body, branchesForNode);
}

function expressionLinesForNode(statement) {
    let expressionLines = [];

    let expressionNodeTypes = ['Expression',
                               'Declaration',
                               'Statement',
                               'Clause',
                               'Literal',
                               'Identifier'];

    if (expressionNodeTypes.some(function(type) {
            return statement.type.indexOf(type) !== -1;
        })) {

        /* These expressions aren't executable on their own */
        switch (statement.type) {
        case 'FunctionDeclaration':
        case 'LiteralExpression':
            break;
        /* Perplexingly, an empty block statement is actually executable,
         * push it if it is */
        case 'BlockStatement':
            if (statement.body.length !== 0)
                break;
            expressionLines.push(statement.loc.start.line);
            break;
        default:
            expressionLines.push(statement.loc.start.line);
            break;
        }
    }

    return expressionLines;
}

function deduplicate(list) {
    return list.filter(function(elem, pos, self) {
        return self.indexOf(elem) === pos;
    });
}

function expressionLinesForAST(ast) {
    let allExpressions = collectForSubNodes(ast.body, expressionLinesForNode);
    allExpressions = deduplicate(allExpressions);

    return allExpressions;
}
