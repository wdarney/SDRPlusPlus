"use strict";
// Country boundaries from public-domain Natural Earth, served inside the app.
function createSDRPPBasicMap() {
    const fill = new ol.style.Fill({color: '#eef0e6'});
    const stroke = new ol.style.Stroke({color: '#87958a', width: 1});
    const styles = new Map();
    // Reuse a rendered image during gestures instead of repainting every polygon.
    // A modest margin keeps short drags inside the cached image.
    return new ol.layer.VectorImage({
        imageRatio: 1.5,
        name: 'sdrpp_basic', title: 'Basic offline map', type: 'base',
        visible: false, background: '#cbdfe9', declutter: true,
        source: new ol.source.Vector({
            url: 'basic-world.geojson', format: new ol.format.GeoJSON(),
            attributions: 'Made with Natural Earth (public domain)',
        }),
        style: function(feature, resolution) {
            const city = feature.get('city');
            if (city && resolution > 156543.034 / Math.pow(2, feature.get('minZoom'))) return;
            const label = city || resolution < 12000 ? feature.get('name') : '';
            const key = (city ? 'city:' : 'country:') + label;
            if (!styles.has(key)) styles.set(key, new ol.style.Style({
                fill: fill, stroke: stroke,
                image: city ? new ol.style.Circle({radius: 2.5, fill: new ol.style.Fill({color: '#46554d'})}) : undefined,
                text: new ol.style.Text({
                    text: label, font: '12px sans-serif', overflow: false,
                    offsetY: city ? -10 : 0,
                    fill: new ol.style.Fill({color: '#46554d'}),
                    stroke: new ol.style.Stroke({color: '#ffffff', width: 3}),
                }),
            }));
            return styles.get(key);
        },
    });
}
