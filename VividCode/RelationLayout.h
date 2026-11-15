#pragma once
#include <QMap>
#include <QSet>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <functional>

struct RelationLayout {
    QMap<QString, QPointF> positions;
    QSet<QString> cyclicGroups;
    QSizeF size;
};

// Links use the displayed direction: referenced group -> referrer group.
// Only visible, real groups are arranged; no analysis facts or edges are added.
inline RelationLayout layoutRelationGroups(const QMap<QString, QSizeF> &sizes,
    const QList<QPair<QString, QString>> &links) {
    RelationLayout result;
    if (sizes.isEmpty()) return result;
    constexpr qreal rowGap = 42, layerGap = 160, componentGap = 90;
    QMap<QString, QSet<QString>> next, adjacent;
    for (const auto &link : links) {
        if (!sizes.contains(link.first) || !sizes.contains(link.second) || link.first == link.second) continue;
        next[link.first].insert(link.second);
        adjacent[link.first].insert(link.second); adjacent[link.second].insert(link.first);
    }
    const auto sorted = [](const QSet<QString> &values) { auto list = values.values(); std::sort(list.begin(), list.end()); return list; };
    // Tarjan: a cycle is one layout block, with its original groups still visible.
    QMap<QString, int> index, low, blockFor;
    QSet<QString> active;
    QStringList stack;
    QList<QStringList> blocks;
    int serial = 0;
    std::function<void(const QString &)> visit = [&](const QString &id) {
        index[id] = low[id] = serial++; stack.append(id); active.insert(id);
        for (const auto &to : sorted(next.value(id))) {
            if (!index.contains(to)) { visit(to); low[id] = qMin(low[id], low[to]); }
            else if (active.contains(to)) low[id] = qMin(low[id], index[to]);
        }
        if (low[id] != index[id]) return;
        QStringList members;
        QString member;
        do { member = stack.takeLast(); active.remove(member); members.append(member); } while (member != id);
        std::sort(members.begin(), members.end());
        for (const auto &value : members) {
            blockFor[value] = int(blocks.size());
            if (members.size() > 1) result.cyclicGroups.insert(value);
        }
        blocks.append(members);
    };
    for (auto it = sizes.begin(); it != sizes.end(); ++it) if (!index.contains(it.key())) visit(it.key());
    QMap<int, QSet<int>> outgoing, incoming;
    for (auto it = next.begin(); it != next.end(); ++it) for (const auto &to : it.value()) {
        const int a = blockFor[it.key()], b = blockFor[to];
        if (a != b) { outgoing[a].insert(b); incoming[b].insert(a); }
    }
    QList<QSizeF> blockSizes;
    QMap<QString, QPointF> insideBlock;
    for (const auto &members : blocks) {
        // Large cyclic regions must stay compact instead of becoming one very tall column.
        const int columns = members.size() <= 3 ? 1 : int(std::ceil(std::sqrt(double(members.size()))));
        const int rows = (int(members.size()) + columns - 1) / columns;
        QList<qreal> widths(columns, 0), heights(rows, 0);
        for (int i = 0; i < members.size(); ++i) {
            widths[i % columns] = qMax(widths[i % columns], sizes[members[i]].width());
            heights[i / columns] = qMax(heights[i / columns], sizes[members[i]].height());
        }
        qreal y = 0, width = 0;
        for (int row = 0; row < rows; ++row) {
            qreal x = 0;
            for (int column = 0; column < columns; ++column) {
                const int i = row * columns + column;
                if (i < members.size()) insideBlock[members[i]] = QPointF(x, y);
                x += widths[column] + layerGap;
            }
            width = x - layerGap; y += heights[row] + rowGap;
        }
        blockSizes.append(QSizeF(width, y - rowGap));
    }
    struct Component { QString key; QMap<QString, QPointF> positions; QSizeF size; int nodeCount = 0; };
    QList<Component> components;
    QSet<QString> seen;
    for (auto it = sizes.begin(); it != sizes.end(); ++it) {
        if (seen.contains(it.key())) continue;
        QStringList pending{it.key()}, members;
        seen.insert(it.key());
        while (!pending.isEmpty()) {
            const auto id = pending.takeFirst(); members.append(id);
            for (const auto &other : sorted(adjacent.value(id))) if (!seen.contains(other)) { seen.insert(other); pending.append(other); }
        }
        QSet<int> componentBlocks;
        for (const auto &id : members) componentBlocks.insert(blockFor[id]);
        QMap<int, int> indegree, rank;
        QList<int> ready;
        for (const int b : componentBlocks) {
            indegree[b] = int(incoming.value(b).size()); rank[b] = 0;
            if (!indegree[b]) ready.append(b);
        }
        const auto byKey = [&](int a, int b) { return blocks[a].first() < blocks[b].first(); };
        while (!ready.isEmpty()) {
            std::sort(ready.begin(), ready.end(), byKey);
            const int b = ready.takeFirst();
            for (const int to : outgoing.value(b)) {
                rank[to] = qMax(rank[to], rank[b] + 1);
                if (--indegree[to] == 0) ready.append(to);
            }
        }
        int maxRank = 0;
        for (const int b : componentBlocks) maxRank = qMax(maxRank, rank[b]);
        QList<QList<int>> layers(maxRank + 1);
        for (const int b : componentBlocks) layers[rank[b]].append(b);
        for (auto &layer : layers) std::sort(layer.begin(), layer.end(), byKey);
        const auto centers = [&] {
            QMap<int, qreal> ys;
            for (const auto &layer : layers) {
                qreal y = 0;
                for (const int b : layer) { ys[b] = y + blockSizes[b].height() / 2; y += blockSizes[b].height() + rowGap; }
            }
            return ys;
        };
        // Keep the best deterministic sweep: fewer crossings first, shorter vertical travel second.
        const auto score = [&] {
            const auto ys = centers();
            QList<QPair<int, int>> edges;
            qreal length = 0;
            for (const int b : componentBlocks) for (const int to : outgoing.value(b)) { edges.append({b, to}); length += qAbs(ys[b] - ys[to]); }
            int crossings = 0;
            for (int a = 0; a < edges.size(); ++a) for (int b = a + 1; b < edges.size(); ++b) {
                const auto e = edges[a], f = edges[b];
                if (rank[e.first] == rank[f.first] && rank[e.second] == rank[f.second] &&
                    (ys[e.first] - ys[f.first]) * (ys[e.second] - ys[f.second]) < 0) ++crossings;
            }
            return QPair<int, qreal>(crossings, length);
        };
        auto best = layers;
        auto bestScore = score();
        for (int sweep = 0; sweep < 8; ++sweep) {
            const bool forward = sweep % 2 == 0;
            for (int step = 0; step <= maxRank; ++step) {
                const int r = forward ? step : maxRank - step;
                const auto ys = centers();
                QMap<int, qreal> barycenter;
                for (const int b : layers[r]) {
                    const auto neighbors = forward ? incoming.value(b) : outgoing.value(b);
                    qreal sum = 0;
                    for (const int other : neighbors) sum += ys[other];
                    barycenter[b] = neighbors.isEmpty() ? ys[b] : sum / neighbors.size();
                }
                std::stable_sort(layers[r].begin(), layers[r].end(), [&](int a, int b) { return barycenter[a] < barycenter[b]; });
            }
            const auto candidate = score();
            if (candidate < bestScore) { bestScore = candidate; best = layers; }
        }
        layers = best;
        qreal fullHeight = 0;
        for (const auto &layer : layers) {
            qreal height = -rowGap;
            for (const int b : layer) height += blockSizes[b].height() + rowGap;
            fullHeight = qMax(fullHeight, height);
        }
        Component component; component.key = it.key(); component.nodeCount = int(members.size());
        qreal x = 0;
        for (const auto &layer : layers) {
            qreal width = 0, height = -rowGap;
            for (const int b : layer) { width = qMax(width, blockSizes[b].width()); height += blockSizes[b].height() + rowGap; }
            qreal y = (fullHeight - height) / 2;
            for (const int b : layer) {
                for (const auto &id : blocks[b]) component.positions[id] = QPointF(x, y) + insideBlock[id];
                y += blockSizes[b].height() + rowGap;
            }
            x += width + layerGap;
        }
        // Same-layer cycle arrows leave the card on the right, loop above it and enter on the left.
        // Reserve their control-point envelope before packing unrelated components beside them.
        QMap<QPair<QString, QString>, int> cyclePairs;
        for (const auto &link : links)
            if (link.first != link.second && component.positions.contains(link.first) && component.positions.contains(link.second) &&
                blockFor[link.first] == blockFor[link.second]) ++cyclePairs[link];
        qreal routingMargin = 0;
        for (auto pair = cyclePairs.begin(); pair != cyclePairs.end(); ++pair)
            routingMargin = qMax(routingMargin, 70.0 + (pair.value() - 1) * 30 + (cyclePairs.contains({pair.key().second, pair.key().first}) ? 45 : 0));
        for (auto pos = component.positions.begin(); pos != component.positions.end(); ++pos) pos.value() += QPointF(routingMargin, routingMargin);
        component.size = QSizeF(x - layerGap + routingMargin * 2, fullHeight + routingMargin); components.append(component);
    }
    // Pack disconnected pieces as separate islands; long chains retain their dependency direction.
    qreal area = 0, widest = 0;
    for (const auto &c : components) { area += (c.size.width() + componentGap) * (c.size.height() + componentGap); widest = qMax(widest, c.size.width()); }
    const qreal shelfWidth = qMax(widest, std::sqrt(area * 1.8));
    std::sort(components.begin(), components.end(), [](const Component &a, const Component &b) {
        if (a.nodeCount != b.nodeCount) return a.nodeCount > b.nodeCount;
        const qreal aa = a.size.width() * a.size.height(), bb = b.size.width() * b.size.height();
        return aa != bb ? aa > bb : a.key < b.key;
    });
    qreal x = 0, y = 0, shelfHeight = 0;
    qreal fullWidth = 0;
    for (const auto &c : components) {
        if (x > 0 && x + c.size.width() > shelfWidth) { x = 0; y += shelfHeight + componentGap; shelfHeight = 0; }
        for (auto it = c.positions.begin(); it != c.positions.end(); ++it) result.positions[it.key()] = it.value() + QPointF(x, y);
        fullWidth = qMax(fullWidth, x + c.size.width());
        x += c.size.width() + componentGap; shelfHeight = qMax(shelfHeight, c.size.height());
    }
    result.size = QSizeF(fullWidth, y + shelfHeight);
    return result;
}
